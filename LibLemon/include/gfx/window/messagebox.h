#pragma once

#define MESSAGEBOX_OK 1
#define MESSAGEBOX_OKCANCEL 2
#define MESSAGEBOX_EXITRETRYIGNORE 4

#define MESSAGEBOX_RETURN_OKIGNORE 0
#define MESSAGEBOX_RETURN_CANCELEXIT 1
#define MESSAGEBOX_RETURN_RETRY 2

namespace Lemon::GUI{
    int MessageBox(const char* message, int flags);
}