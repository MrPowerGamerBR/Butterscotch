#include "options.h"

#include <string.h>
#include <stdio.h>

#define OPTION_STATUS_DONE ((OptionsStatus){ true, false, 0, nullptr })
#define OPTION_STATUS_ERROR ((OptionsStatus){ true, true, 0, nullptr })
#define OPTION_STATUS_FLAG(k) ((OptionsStatus){ false, false, k, nullptr })
#define OPTION_STATUS_VALUE(k, v) ((OptionsStatus){ false, false, k, v })

void Options_init(Options *options, const struct Option *optionsList, const char **argv) {
    options->list = optionsList;
    options->argv = argv;
    options->index = 1;
}

// No flag bundle atm
OptionsStatus Options_getValue(Options* options) {
    const char *query = options->argv[options->index];
    const Option *option = nullptr;
    const char *value = nullptr;

    if (query == nullptr) {
        return OPTION_STATUS_DONE;
    }

    size_t queryLen = strlen(query);
    value = strchr(query, '=');
    if (value != nullptr) {
        if (value[1] == '\0') {
            value = nullptr;
        } else {
            queryLen = value - query;
            value++;
        }
    }

    // Eh... using shortName as sentinel?
    for (int i = 0; options->list[i].shortName != 0; i++) {
        if (query[0] == '-' && query[1] == '-' && query[2]) {
            if (strncmp(options->list[i].longName, query + 2, queryLen - 2) == 0) {
                option = &options->list[i];
                break;
            }
        } else if (query[0] == '-' && query[1]) {
            if (query[2] != '\0' && query[2] != '=') {
                fprintf(stderr, "Error: invalid option '%s'\n", query);
                return OPTION_STATUS_ERROR;
            } else if (options->list[i].shortName == query[1]) {
                option = &options->list[i];
                break;
            }
        }
    }

    if (option == nullptr) {
        if (query[0] == '-') {
            fprintf(stderr, "Error: unknown option '%.*s'\n", (int)queryLen, query);
            return OPTION_STATUS_ERROR;
        }
        return OPTION_STATUS_DONE;
    }
    options->index++;

    switch (option->type) {
        case OPTION_TYPE_VALUE:
            if (value == nullptr) {
                value = options->argv[options->index++];
            }

            if (value == nullptr) {
                fprintf(stderr, "Error: missing argument for option '%.*s'\n", (int)queryLen, query);
                return OPTION_STATUS_ERROR;
            }
            return OPTION_STATUS_VALUE(option->shortName, value);
        case OPTION_TYPE_FLAG:
            if (value != nullptr) {
                fprintf(stderr, "Error: flag option '%.*s' does not take an argument\n", (int)queryLen, query);
                return OPTION_STATUS_ERROR;
            }

            return OPTION_STATUS_FLAG(option->shortName);
    }
}
