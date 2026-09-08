#include "gml_method.h"
#include "common.h"
#include "data_win.h"
#include "utils.h"
#include <stdlib.h>

char* GMLMethod_toString(const GMLMethod* method, DataWin* dataWin) {
    if (method == nullptr) return safeStrdup("<method:null>");
    if (method->unresolvedName != nullptr && method->unresolvedName[0] != '\0') {
        return safeStrdup(method->unresolvedName);
    }
    if (dataWin != nullptr && method->codeIndex >= 0 && (uint32_t) method->codeIndex < dataWin->func.functionCount) {
        const char* name = dataWin->func.functions[method->codeIndex].name;
        if (name != nullptr && name[0] != '\0') {
            return safeStrdup(name);
        }
    }
    if (method->builtin != nullptr) {
        return safeStrdup("<builtin>");
    }
    if (method->codeIndex >= 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "<method:%d>", method->codeIndex);
        return safeStrdup(buf);
    }
    return safeStrdup("<method>");
}

GMLMethod* GMLMethod_create(int32_t codeIndex, int32_t boundInstanceId) {
    GMLMethod* m = (GMLMethod *)safeCalloc(1, sizeof(GMLMethod));
    m->refCount = 1;
    m->codeIndex = codeIndex;
    m->boundInstanceId = boundInstanceId;
    return m;
}

GMLMethod* GMLMethod_createBuiltin(BuiltinFunc builtin, int32_t boundInstanceId) {
    GMLMethod* m = (GMLMethod *)safeCalloc(1, sizeof(GMLMethod));
    m->refCount = 1;
    m->codeIndex = -1;
    m->boundInstanceId = boundInstanceId;
    m->builtin = builtin;
    return m;
}

GMLMethod* GMLMethod_createUnresolved(const char* name, int32_t boundInstanceId) {
    GMLMethod* m = (GMLMethod *)safeCalloc(1, sizeof(GMLMethod));
    m->refCount = 1;
    m->codeIndex = -1;
    m->boundInstanceId = boundInstanceId;
    m->unresolvedName = name;
    return m;
}

void GMLMethod_incRef(GMLMethod* m) {
    if (m == nullptr) return;
    m->refCount++;
}

void GMLMethod_decRef(GMLMethod* m) {
    if (m == nullptr) return;
    require(m->refCount > 0);
    m->refCount--;
    if (m->refCount > 0) return;
    free(m);
}
