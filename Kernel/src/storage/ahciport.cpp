#include <ahci.h>

#include <physicalallocator.h>
#include <paging.h>
#include <logging.h>
#include <gpt.h>
#include <ata.h>
#include <timer.h>

#define HBA_PxIS_TFES   (1 << 30)

namespace AHCI{
	Port::Port(int num, hba_port_t* portStructure, hba_mem_t* hbaMem){
        Log::Info("name %s", this->name);
        registers = portStructure;

		registers->cmd &= ~HBA_PxCMD_ST;
		registers->cmd &= ~HBA_PxCMD_FRE;

        stopCMD(registers);

        uintptr_t phys;
        
		// Command list entry size = 32
		// Command list entry maxim count = 32
		// Command list maxim size = 32*32 = 1K per port
        phys = Memory::AllocatePhysicalMemoryBlock();
		registers->clb = (uint32_t)(phys & 0xFFFFFFFF);
		registers->clbu = (uint32_t)(phys >> 32);
	
		// FIS entry size = 256 bytes per port
        phys = Memory::AllocatePhysicalMemoryBlock();
		registers->fb = (uint32_t)(phys & 0xFFFFFFFF);
		registers->fbu = (uint32_t)(phys >> 32);
	
		// Command list size = 256*32 = 8K per port
		commandList = (hba_cmd_header_t*)Memory::KernelAllocate4KPages(1);
        Memory::KernelMapVirtualMemory4K(static_cast<uintptr_t>(registers->clb) + (static_cast<uintptr_t>(registers->clbu) << 32), (uintptr_t)commandList, 1);
        memset(commandList, 0, PAGE_SIZE_4K);

		// FIS
		fis = Memory::KernelAllocate4KPages(1);
        Memory::KernelMapVirtualMemory4K(static_cast<uintptr_t>(registers->fb) + (static_cast<uintptr_t>(registers->fbu) << 32), (uintptr_t)fis, 1);
        memset(fis, 0, PAGE_SIZE_4K);

        for(int i = 0; i < 8 /*Support for 8 command slots*/; i++){
            commandList[i].prdtl = 1;

            phys = Memory::AllocatePhysicalMemoryBlock();
            commandList[i].ctba = (uint32_t)(phys & 0xFFFFFFFF);
            commandList[i].ctbau = (uint32_t)(phys >> 32);

            commandTables[i] = (hba_cmd_tbl_t*)Memory::KernelAllocate4KPages(1);
            Memory::KernelMapVirtualMemory4K(phys,(uintptr_t)commandTables[i], 1);
            memset(commandTables[i],0,PAGE_SIZE_4K);
        }

        registers->sctl |= (SCTL_PORT_IPM_NOPART | SCTL_PORT_IPM_NOSLUM | SCTL_PORT_IPM_NODSLP);

        if(hbaMem->cap & AHCI_CAP_SALP){
            registers->cmd &= ~HBA_PxCMD_ASP; // Disable aggressive slumber and partial
        }

        registers->is = 0; // Clear interrupts
        registers->ie = 1;
        registers->fbs &= ~(0xFFFFF000U);

        registers->cmd |= HBA_PxCMD_POD;
        registers->cmd |= HBA_PxCMD_SUD;

        {
            int spin = 0;
            while(spin++ < 100 && (registers->ssts & HBA_PxSSTS_DET) != HBA_PxSSTS_DET_PRESENT){
                Timer::Wait(1);
            }

            if((registers->ssts & HBA_PxSSTS_DET) != HBA_PxSSTS_DET_PRESENT){
                Log::Info("[AHCI] Device not present (DET: %x)", registers->ssts & HBA_PxSSTS_DET);
                return;
            }
        }

        registers->cmd = (registers->cmd & ~HBA_PxCMD_ICC) | HBA_PxCMD_ICC_ACTIVE;

        startCMD(registers);

        bufPhys = Memory::AllocatePhysicalMemoryBlock();
        bufVirt = Memory::KernelAllocate4KPages(1);
        Memory::KernelMapVirtualMemory4K(bufPhys, (uintptr_t)bufVirt, 1);

        Log::Info("[AHCI] Port - SSTS: %x, SCTL: %x, SERR: %x, SACT: %x, Cmd/Status: %x, FBS: %x, IE: %x", registers->ssts, registers->sctl, registers->serr, registers->sact, registers->cmd, registers->fbs, registers->ie);

        switch(GPT::Parse(this)){
        case 0:
            Log::Error("[SATA] Disk has a corrupted or non-existant GPT. MBR disks are NOT supported.");
            break;
        case -1:
            Log::Error("[SATA] Disk Error while Parsing GPT for SATA Disk ");
            break;
        }

        InitializePartitions();
    }

    int Port::ReadDiskBlock(uint64_t lba, uint32_t count, void* buffer){
        uint64_t blockCount = ((count + 511) / 512);
        
        while(blockCount >= 8 && count){
            uint64_t size;
            if(count < 512 * 8) size = count;
            else size = 512 * 8;

            if(!size) continue;

            if(Access(lba, 8, 0)){ // LBA, 2 blocks, read
                return 1; // Error Reading Sectors
            }

            memcpy(buffer, bufVirt, size);
            buffer += size;
            lba += 8;
            blockCount -= 8;
            count -= size;
        }

        while(blockCount >= 2 && count){
            uint64_t size;
            if(count < 512 * 2) size = count;
            else size = 512 * 2;

            if(!size) continue;

            if(Access(lba, 2, 0)){ // LBA, 2 blocks, read
                return 1; // Error Reading Sectors
            }

            memcpy(buffer, bufVirt, size);
            buffer += size;
            lba += 2;
            blockCount -= 2;
            count -= size;
        }

        while(blockCount-- && count){
            uint64_t size;
            if(count < 512) size = count;
            else size = 512;

            if(!size) continue;

            if(Access(lba, 1, 0)){ // LBA, 1 block, read
                return 1; // Error Reading Sectors
            }

            memcpy(buffer, bufVirt, size);
            buffer += size;
            lba++;
        }

        return 0;
    }

    
    int Port::WriteDiskBlock(uint64_t lba, uint32_t count, void* buffer){
        uint64_t blockCount = ((count / 512 * 512) < count) ? ((count / 512) + 1) : (count / 512);

        while(blockCount-- && count){
            uint64_t size;
            if(count < 512) size = count;
            else size = 512;

            if(!size) continue;

            memcpy(bufVirt, buffer, size);

            if(Access(lba, 1, 1)){ // LBA, 1 block, write
                return 1; // Error Reading Sectors
            }

            buffer += size;
            lba++;
        }

        return 0;
    }

    int Port::Access(uint64_t lba, uint32_t count, int write){
        registers->ie = 0xffffffff; 
        registers->is = 0xffffffff; 
        int spin = 0;

        registers->tfd = 0;

        int slot = FindCmdSlot();
        if(slot == -1){
            Log::Warning("[SATA] Could not find command slot!");
            return 2;
        }

        hba_cmd_header_t* commandHeader = &commandList[slot];

        commandHeader->cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);

        commandHeader->a = 0;
        commandHeader->w = write;
        commandHeader->c = 1;
        commandHeader->p = 0;
        commandHeader->r = 1;

        commandHeader->prdbc = 0;
        commandHeader->pmp = 0;

        hba_cmd_tbl_t* commandTable = commandTables[slot];
        memset(commandTable, 0, sizeof(hba_cmd_tbl_t));

        commandTable->prdt_entry[0].dba = bufPhys & 0xFFFFFFFF;
        commandTable->prdt_entry[0].dbau = (bufPhys >> 32) & 0xFFFFFFFF;
        commandTable->prdt_entry[0].dbc = 512 * count - 1; // 512 bytes per sector
        commandTable->prdt_entry[0].i = 1;

        fis_reg_h2d_t* cmdfis = (fis_reg_h2d_t*)(commandTable->cfis); 
        memset(commandTable->cfis, 0, sizeof(fis_reg_h2d_t));

        cmdfis->fis_type = FIS_TYPE_REG_H2D;
        cmdfis->c = 1;  // Command
        cmdfis->pmport = 0; // Port multiplier

        if(write){
            cmdfis->command = ATA_CMD_WRITE_DMA_EX;
        } else {
            cmdfis->command = ATA_CMD_READ_DMA_EX;
        }
 
        cmdfis->lba0 = lba & 0xFF;
        cmdfis->lba1 = (lba >> 8) & 0xFF;
        cmdfis->lba2 = (lba >> 16) & 0xFF;
        cmdfis->device = 1 << 6;
 
        cmdfis->lba3 = (lba >> 24) & 0xFF;
        cmdfis->lba4 = (lba >> 32) & 0xFF;
        cmdfis->lba5 = (lba >> 40) & 0xFF;
 
        cmdfis->countl = count & 0xff;
        cmdfis->counth = count >> 8;

        cmdfis->control = 0;

        while ((registers->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) && spin < 1000000) {
            spin++;
        }

        if(spin >= 1000000){
            Log::Warning("[SATA] Port Hung");
            return 3;
        }

        registers->ci |= 1 << slot;

        Log::Info("SERR: %x, Slot: %x, PxCMD: %x, Int status: %x, Ci: %x, TFD: %x", registers->serr, slot, registers->cmd, registers->is, registers->ci, registers->tfd);

        while(registers->ci & (1 << slot)) {
            if (registers->is & HBA_PxIS_TFES)   // Task file error
            {
                Log::Warning("[SATA] Disk Error (SERR: %x)", registers->serr);
                return 1;
            }
        }
        
        Log::Info("SERR: %x, Slot: %x, PxCMD: %x, Int status: %x, Ci: %x, TFD: %x", registers->serr, slot, registers->cmd, registers->is, registers->ci, registers->tfd);
        
        if (registers->is & HBA_PxIS_TFES) {
            Log::Warning("[SATA] Disk Error (SERR: %x)", registers->serr);
            return 1;
        }
        return 0;
    }

    int Port::FindCmdSlot(){
        // If not set in SACT and CI, the slot is free
        uint32_t slots = (registers->sact | registers->ci);
        for (int i=0; i<8; i++)
        {
            if ((slots&1) == 0)
                return i;
            slots >>= 1;
        }

        return -1;
    }
}


