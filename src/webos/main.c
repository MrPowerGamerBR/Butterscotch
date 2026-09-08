#include <loop.h>

#include "platformdefs.h"

int main(int argc, char **argv)
{
    (void)argc;

    CommandLineArgs args = {0};

    args.exitAtFrame = -1;

#ifdef ENABLE_VM_TRACING
    args.traceBytecodeAfterFrame = 0;
#endif

    args.speedMultiplier = 1.0;
    args.fastForwardSpeed = 0.0;

    /*
     * webOS native apps receive a JSON launch object in argv[1].
     * It is not a data.win path, so deliberately ignore it here.
     */
    args.osType = OS_WINDOWS;
    args.profilerFramesBetween = 0;
    args.loadType = DATAWINLOADTYPE_LOAD_IN_MEMORY_AHEAD_OF_TIME;
    args.renderer = MODERN_GL;

    args.dataWinPath = "./data.win";
    args.saveFolder = "./saves";

    int ret = loop(args, argv[0]);

    freeCommandLineArgs(&args);
    return ret;
}
