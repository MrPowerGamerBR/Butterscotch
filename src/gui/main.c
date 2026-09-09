#include <cli/command_line_args.h>
#include <loop.h>

/* For SDL_main */
#if defined(USE_SDL1)
#include <SDL/SDL_main.h>
#elif defined(USE_SDL2)
#include <SDL2/SDL_main.h>
#elif defined(USE_SDL3)
#include <SDL3/SDL_main.h>
#endif

int guiMainImpl(int argc, char* argv[]) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    CommandLineArgs args;
    parseCommandLineArgs(&args, argc, argv, true);
    setLogColours(!args.disableLogColours);
    int ret = loop(args, argv[0]);
    freeCommandLineArgs(&args);
    return ret;
}