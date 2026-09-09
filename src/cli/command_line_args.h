#ifndef _BS_COMMAND_LINE_ARGS_H_
#define _BS_COMMAND_LINE_ARGS_H_

#include "platformdefs.h"

void parseCommandLineArgs(CommandLineArgs* args, int argc, char* argv[], bool allowMissingDataWinPath);
void setLogColours(bool enabled);

#endif /* _BS_COMMAND_LINE_ARGS_H_ */