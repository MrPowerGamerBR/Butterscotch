#pragma once

typedef enum OptionType {
    OPTION_TYPE_FLAG,
    OPTION_TYPE_VALUE,
} OptionType;

typedef struct Option {
    char shortName;
    OptionType type;
    const char* longName;
} Option;

typedef struct Options {
    const struct Option* list;
    const char **argv;
    int index;
} Options;

typedef struct OptionsStatus {
    bool done;
    bool error;
    char shortName;
    const char *value;
} OptionsStatus;

#define OPTION_SENTINEL ((Option){ 0, 0, nullptr })

void Options_init(Options *options, const struct Option *optionsList, const char *argv[]);
OptionsStatus Options_getValue(Options* options);
