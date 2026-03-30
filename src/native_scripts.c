#include "native_scripts.h"
#include "data_win.h"
#include "instance.h"
#include "renderer.h"
#include "rvalue.h"
#include "text_utils.h"
#include "utils.h"
#include "vm.h"
#include "vm_builtins.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_ds.h"

// ===[ Native Override Registry ]===
// Maps code entry name -> native function pointer
static struct { char* key; NativeCodeFunc value; }* nativeOverrideMap = nullptr;

static void registerNative(const char* codeName, NativeCodeFunc func) {
    shput(nativeOverrideMap, (char*) codeName, func);
}

NativeCodeFunc NativeScripts_find(const char* codeName) {
    ptrdiff_t idx = shgeti(nativeOverrideMap, (char*) codeName);
    if (0 > idx) return nullptr;
    return nativeOverrideMap[idx].value;
}

// ===[ Helper: Find self variable ID by name from VARI chunk ]===
static int32_t findSelfVarId(DataWin* dw, const char* name) {
    forEach(Variable, v, dw->vari.variables, dw->vari.variableCount) {
        if (v->varID >= 0 && v->instanceType != INSTANCE_GLOBAL && strcmp(v->name, name) == 0) {
            return v->varID;
        }
    }
    fprintf(stderr, "NativeScripts: WARNING - self variable '%s' not found in VARI chunk\n", name);
    return -1;
}

// ===[ Helper: Find global variable ID by name ]===
static int32_t findGlobalVarId(VMContext* ctx, const char* name) {
    ptrdiff_t idx = shgeti(ctx->globalVarNameMap, (char*) name);
    if (0 > idx) {
        fprintf(stderr, "NativeScripts: WARNING - global variable '%s' not found\n", name);
        return -1;
    }
    return ctx->globalVarNameMap[idx].value;
}

// ===[ Helper: Find resource index by name ]===
static int32_t findFontIndex(DataWin* dw, const char* name) {
    repeat(dw->font.count, i) {
        if (strcmp(dw->font.fonts[i].name, name) == 0) return (int32_t) i;
    }
    fprintf(stderr, "NativeScripts: WARNING - font '%s' not found\n", name);
    return -1;
}

static int32_t findObjectIndex(DataWin* dw, const char* name) {
    repeat(dw->objt.count, i) {
        if (strcmp(dw->objt.objects[i].name, name) == 0) return (int32_t) i;
    }
    fprintf(stderr, "NativeScripts: WARNING - object '%s' not found\n", name);
    return -1;
}

// ===[ Helper: Find script code ID by name ]===
static int32_t findScriptCodeId(VMContext* ctx, const char* name) {
    ptrdiff_t idx = shgeti(ctx->funcMap, (char*) name);
    if (0 > idx) {
        fprintf(stderr, "NativeScripts: WARNING - script '%s' not found in funcMap\n", name);
        return -1;
    }
    return ctx->funcMap[idx].value;
}

// ===[ Helper: Set direction with normalization (matches GML behavior) ]===
static void setDirection(Instance* inst, GMLReal value) {
    GMLReal d = GMLReal_fmod(value, 360.0);
    if (0.0 > d) d += 360.0;
    inst->direction = d;
    Instance_computeComponentsFromSpeed(inst);
}

// ===[ Helper: Read self variable as real ]===
static GMLReal selfReal(Instance* inst, int32_t varId) {
    return RValue_toReal(Instance_getSelfVar(inst, varId));
}

// ===[ Helper: Read self variable as int32 ]===
static int32_t selfInt(Instance* inst, int32_t varId) {
    return RValue_toInt32(Instance_getSelfVar(inst, varId));
}

// ===[ Helper: Read self variable as string (non-owning pointer, do NOT free) ]===
static const char* selfString(Instance* inst, int32_t varId) {
    RValue val = Instance_getSelfVar(inst, varId);
    if (val.type == RVALUE_STRING && val.string != nullptr) return val.string;
    return "";
}

// ===[ Helper: Read self array variable ]===
static RValue selfArrayGet(Instance* inst, int32_t varId, int32_t index) {
    int64_t k = ((int64_t) varId << 32) | (uint32_t) index;
    ptrdiff_t idx = hmgeti(inst->selfArrayMap, k);
    if (0 > idx) return RValue_makeReal(0.0);
    RValue result = inst->selfArrayMap[idx].value;
    result.ownsString = false;
    return result;
}

// ===[ Helper: Read global variable as real ]===
static GMLReal globalReal(VMContext* ctx, int32_t varId) {
    if (0 > varId || (uint32_t) varId >= ctx->globalVarCount) return 0.0;
    return RValue_toReal(ctx->globalVars[varId]);
}

// ===[ Helper: Read global variable as string ]===
static const char* globalString(VMContext* ctx, int32_t varId) {
    if (0 > varId || (uint32_t) varId >= ctx->globalVarCount) return "";
    RValue val = ctx->globalVars[varId];
    if (val.type == RVALUE_STRING && val.string != nullptr) return val.string;
    return "";
}

// ===[ Helper: Set global variable ]===
static void globalSet(VMContext* ctx, int32_t varId, RValue val) {
    if (0 > varId || (uint32_t) varId >= ctx->globalVarCount) return;
    RValue_free(&ctx->globalVars[varId]);
    if (val.type == RVALUE_STRING && val.string != nullptr) {
        ctx->globalVars[varId] = RValue_makeOwnedString(safeStrdup(val.string));
    } else {
        ctx->globalVars[varId] = val;
    }
}

// ===[ Helper: Write to global array ]===
static void globalArraySet(VMContext* ctx, int32_t varId, int32_t index, RValue val) {
    int64_t k = ((int64_t) varId << 32) | (uint32_t) index;
    ptrdiff_t idx = hmgeti(ctx->globalArrayMap, k);
    if (idx >= 0) {
        RValue_free(&ctx->globalArrayMap[idx].value);
    }
    if (val.type == RVALUE_STRING && !val.ownsString && val.string != nullptr) {
        val = RValue_makeOwnedString(safeStrdup(val.string));
    }
    hmput(ctx->globalArrayMap, k, val);
}

// ===[ Helper: Call a builtin function ]===
static RValue callBuiltin(VMContext* ctx, const char* name, RValue* args, int32_t argCount) {
    BuiltinFunc func = VMBuiltins_find(name);
    if (func == nullptr) {
        fprintf(stderr, "NativeScripts: builtin '%s' not found\n", name);
        return RValue_makeUndefined();
    }
    return func(ctx, args, argCount);
}

// ===[ Cached IDs for obj_base_writer Draw_0 ]===
static struct {
    bool initialized;
    // Self variable IDs
    int32_t vtext, writingxend, vspacing, writingx, writingy;
    int32_t stringpos, originalstring, mycolor, myfont, shake;
    int32_t halt, stringno, mystring, textspeed, spacing;
    int32_t htextscale, vtextscale, myx, myy;
    // Global variable IDs
    int32_t gFlag, gFaceemotion, gFacechoice, gFacechange, gTyper, gLanguage;
    // Font resource indices
    int32_t fntPapyrus, fntJaPapyrusBtl, fntJaMain, fntJaMaintext;
    int32_t fntMain, fntMaintext, fntComicsans, fntJaComicsans, fntJaComicsansBig;
    int32_t fntJaPapyrus;
    // Object resource indices
    int32_t objPapdate;
    // Script code IDs
    int32_t scrTexttype, scrNewline, scrReplaceButtonsPc, scrGetbuttonsprite, scrSetfont;
} writerCache;

static void initWriterCache(VMContext* ctx, DataWin* dw) {
    if (writerCache.initialized) return;
    writerCache.initialized = true;

    // Self variable IDs
    writerCache.vtext = findSelfVarId(dw, "vtext");
    writerCache.writingxend = findSelfVarId(dw, "writingxend");
    writerCache.vspacing = findSelfVarId(dw, "vspacing");
    writerCache.writingx = findSelfVarId(dw, "writingx");
    writerCache.writingy = findSelfVarId(dw, "writingy");
    writerCache.stringpos = findSelfVarId(dw, "stringpos");
    writerCache.originalstring = findSelfVarId(dw, "originalstring");
    writerCache.mycolor = findSelfVarId(dw, "mycolor");
    writerCache.myfont = findSelfVarId(dw, "myfont");
    writerCache.shake = findSelfVarId(dw, "shake");
    writerCache.halt = findSelfVarId(dw, "halt");
    writerCache.stringno = findSelfVarId(dw, "stringno");
    writerCache.mystring = findSelfVarId(dw, "mystring");
    writerCache.textspeed = findSelfVarId(dw, "textspeed");
    writerCache.spacing = findSelfVarId(dw, "spacing");
    writerCache.htextscale = findSelfVarId(dw, "htextscale");
    writerCache.vtextscale = findSelfVarId(dw, "vtextscale");
    writerCache.myx = findSelfVarId(dw, "myx");
    writerCache.myy = findSelfVarId(dw, "myy");

    // Global variable IDs
    writerCache.gFlag = findGlobalVarId(ctx, "flag");
    writerCache.gFaceemotion = findGlobalVarId(ctx, "faceemotion");
    writerCache.gFacechoice = findGlobalVarId(ctx, "facechoice");
    writerCache.gFacechange = findGlobalVarId(ctx, "facechange");
    writerCache.gTyper = findGlobalVarId(ctx, "typer");
    writerCache.gLanguage = findGlobalVarId(ctx, "language");

    // Font resource indices
    writerCache.fntPapyrus = findFontIndex(dw, "fnt_papyrus");
    writerCache.fntJaPapyrusBtl = findFontIndex(dw, "fnt_ja_papyrus_btl");
    writerCache.fntJaMain = findFontIndex(dw, "fnt_ja_main");
    writerCache.fntJaMaintext = findFontIndex(dw, "fnt_ja_maintext");
    writerCache.fntMain = findFontIndex(dw, "fnt_main");
    writerCache.fntMaintext = findFontIndex(dw, "fnt_maintext");
    writerCache.fntComicsans = findFontIndex(dw, "fnt_comicsans");
    writerCache.fntJaComicsans = findFontIndex(dw, "fnt_ja_comicsans");
    writerCache.fntJaComicsansBig = findFontIndex(dw, "fnt_ja_comicsans_big");
    writerCache.fntJaPapyrus = findFontIndex(dw, "fnt_ja_papyrus");

    // Object resource indices
    writerCache.objPapdate = findObjectIndex(dw, "obj_papdate");

    // Script code IDs
    writerCache.scrTexttype = findScriptCodeId(ctx, "SCR_TEXTTYPE");
    writerCache.scrNewline = findScriptCodeId(ctx, "SCR_NEWLINE");
    writerCache.scrReplaceButtonsPc = findScriptCodeId(ctx, "scr_replace_buttons_pc");
    writerCache.scrGetbuttonsprite = findScriptCodeId(ctx, "scr_getbuttonsprite");
    writerCache.scrSetfont = findScriptCodeId(ctx, "scr_setfont");
}

// ===[ Helper: string_char_at equivalent (1-based, returns single byte as owned string) ]===
static char nativeStringCharAtBuf(const char* str, int32_t pos) {
    int32_t strLen = (int32_t) strlen(str);
    pos--; // 1-based to 0-based
    if (0 > pos || pos >= strLen) return '\0';
    return str[pos];
}

// ===[ Native implementation: gml_Object_obj_base_writer_Draw_0 ]===
static void native_objBaseWriter_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    Renderer* renderer = runner->renderer;
    if (renderer == nullptr) return;

    DataWin* dw = ctx->dataWin;

    // Read instance variables
    int32_t vtext = selfInt(inst, writerCache.vtext);
    GMLReal writingxend = selfReal(inst, writerCache.writingxend);
    GMLReal vspacing = selfReal(inst, writerCache.vspacing);
    GMLReal writingx = selfReal(inst, writerCache.writingx);
    GMLReal writingy = selfReal(inst, writerCache.writingy);
    int32_t stringpos = selfInt(inst, writerCache.stringpos);
    const char* originalstring = selfString(inst, writerCache.originalstring);
    int32_t mycolor = selfInt(inst, writerCache.mycolor);
    int32_t myfont = selfInt(inst, writerCache.myfont);
    GMLReal shake = selfReal(inst, writerCache.shake);
    GMLReal spacing = selfReal(inst, writerCache.spacing);
    GMLReal htextscale = selfReal(inst, writerCache.htextscale);
    GMLReal vtextscale = selfReal(inst, writerCache.vtextscale);

    // myx and myy are read and written throughout, so we track them locally.
    // IMPORTANT: we must also write them to the instance immediately, because sub-scripts
    // (like SCR_NEWLINE) read myx/myy from the instance's self vars. If we only write them
    // back at the end, sub-scripts see stale values from the previous frame.
    GMLReal myx, myy;
    if (vtext) {
        myx = writingxend - vspacing;
    } else {
        myx = writingx;
    }
    myy = writingy;
    Instance_setSelfVar(inst, writerCache.myx, RValue_makeReal(myx));
    Instance_setSelfVar(inst, writerCache.myy, RValue_makeReal(myy));

    int32_t halfsize = 0;
    const char* language = globalString(ctx, writerCache.gLanguage);
    bool isEnglish = (strcmp(language, "en") == 0);
    bool isJapanese = (strcmp(language, "ja") == 0);

    // We need a mutable copy of originalstring for the % case
    // Track whether we replaced it (to know if we need to free)
    char* ownedOriginalString = nullptr;

    for (int32_t n = 1; stringpos >= n; n++) {
        char ch = nativeStringCharAtBuf(originalstring, n);
        if (ch == '\0') break;

        if (ch == '^' && nativeStringCharAtBuf(originalstring, n + 1) != '0') {
            // Skip caret + next char
            n++;
        } else if (ch == '\\') {
            n++;
            ch = nativeStringCharAtBuf(originalstring, n);
            if (ch == 'R') {
                mycolor = 255;
            } else if (ch == 'G') {
                mycolor = 65280;
            } else if (ch == 'W') {
                mycolor = 16777215;
            } else if (ch == 'Y') {
                mycolor = 65535;
            } else if (ch == 'X') {
                mycolor = 0;
            } else if (ch == 'B') {
                mycolor = 16711680;
            } else if (ch == 'O') {
                mycolor = 4235519;
            } else if (ch == 'L') {
                mycolor = 16629774;
            } else if (ch == 'P') {
                mycolor = 16711935;
            } else if (ch == 'p') {
                mycolor = 13941759;
            } else if (ch == 'C') {
                // event_user(1)
                Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_USER0 + 1);
            } else if (ch == 'M') {
                n++;
                ch = nativeStringCharAtBuf(originalstring, n);
                GMLReal val = 0.0;
                if (ch != '\0') {
                    char buf[2] = { ch, '\0' };
                    val = GMLReal_strtod(buf, nullptr);
                }
                // global.flag[20] = real(ch)
                globalArraySet(ctx, writerCache.gFlag, 20, RValue_makeReal(val));
            } else if (ch == 'E') {
                n++;
                ch = nativeStringCharAtBuf(originalstring, n);
                GMLReal val = 0.0;
                if (ch != '\0') {
                    char buf[2] = { ch, '\0' };
                    val = GMLReal_strtod(buf, nullptr);
                }
                globalSet(ctx, writerCache.gFaceemotion, RValue_makeReal(val));
            } else if (ch == 'F') {
                n++;
                ch = nativeStringCharAtBuf(originalstring, n);
                GMLReal val = 0.0;
                if (ch != '\0') {
                    char buf[2] = { ch, '\0' };
                    val = GMLReal_strtod(buf, nullptr);
                }
                globalSet(ctx, writerCache.gFacechoice, RValue_makeReal(val));
                globalSet(ctx, writerCache.gFacechange, RValue_makeReal(1.0));
            } else if (ch == 'S') {
                n++;
            } else if (ch == 'T') {
                n++;
                char newtyper = nativeStringCharAtBuf(originalstring, n);
                if (newtyper == '-') {
                    halfsize = 1;
                } else if (newtyper == '+') {
                    halfsize = 0;
                } else {
                    int32_t typerVal = 0;
                    bool setTyper = true;
                    if (newtyper == 'T') typerVal = 4;
                    else if (newtyper == 't') typerVal = 48;
                    else if (newtyper == '0') typerVal = 5;
                    else if (newtyper == 'S') typerVal = 10;
                    else if (newtyper == 'F') typerVal = 16;
                    else if (newtyper == 's') typerVal = 17;
                    else if (newtyper == 'P') typerVal = 18;
                    else if (newtyper == 'M') typerVal = 27;
                    else if (newtyper == 'U') typerVal = 37;
                    else if (newtyper == 'A') typerVal = 47;
                    else if (newtyper == 'a') typerVal = 60;
                    else if (newtyper == 'R') typerVal = 76;
                    else setTyper = false;

                    if (setTyper) {
                        globalSet(ctx, writerCache.gTyper, RValue_makeReal((GMLReal) typerVal));
                    }

                    // SCR_TEXTTYPE(global.typer)
                    GMLReal currentTyper = globalReal(ctx, writerCache.gTyper);
                    RValue scrArg = RValue_makeReal(currentTyper);
                    RValue scrResult = VM_callCodeIndex(ctx, writerCache.scrTexttype, &scrArg, 1);
                    RValue_free(&scrResult);

                    globalSet(ctx, writerCache.gFacechange, RValue_makeReal(1.0));
                }
            } else if (ch == 'z') {
                n++;
                char symCh = nativeStringCharAtBuf(originalstring, n);
                GMLReal sym = 0.0;
                if (symCh != '\0') {
                    char buf[2] = { symCh, '\0' };
                    sym = GMLReal_strtod(buf, nullptr);
                }
                if ((int32_t) sym == 4) {
                    int32_t symS = 862;
                    GMLReal rshake = ((GMLReal) rand() / (GMLReal) RAND_MAX) * shake;
                    GMLReal rshake2 = ((GMLReal) rand() / (GMLReal) RAND_MAX) * shake;
                    Renderer_drawSpriteExt(renderer, symS, 0,
                        (float) (myx + (rshake - (shake / 2.0))),
                        (float) (myy + 10.0 + (rshake2 - (shake / 2.0))),
                        2.0f, 2.0f, 0.0f, 0xFFFFFF, 1.0f);
                }
            } else if (ch == '*') {
                n++;
                ch = nativeStringCharAtBuf(originalstring, n);
                int32_t icontype = 0;
                if (myfont == writerCache.fntPapyrus || myfont == writerCache.fntJaPapyrusBtl) {
                    icontype = 1;
                }
                // scr_getbuttonsprite(ch, icontype)
                char chStr[2] = { ch, '\0' };
                RValue getbtnArgs[2] = { RValue_makeString(chStr), RValue_makeReal((GMLReal) icontype) };
                RValue spriteResult = VM_callCodeIndex(ctx, writerCache.scrGetbuttonsprite, getbtnArgs, 2);
                int32_t sprite = RValue_toInt32(spriteResult);
                RValue_free(&spriteResult);

                if (sprite != -4) {
                    GMLReal spritex = myx;
                    GMLReal spritey = myy;
                    if (shake > 38) {
                        if ((int32_t) shake == 39) {
                            setDirection(inst, inst->direction + 10);
                            spritex += inst->hspeed;
                            spritey += inst->vspeed;
                        } else if ((int32_t) shake == 40) {
                            spritex += inst->hspeed;
                            spritey += inst->vspeed;
                        } else if ((int32_t) shake == 41) {
                            setDirection(inst, inst->direction + (10.0 * n));
                            spritex += inst->hspeed;
                            spritey += inst->vspeed;
                            setDirection(inst, inst->direction - (10.0 * n));
                        } else if ((int32_t) shake == 42) {
                            setDirection(inst, inst->direction + (20.0 * n));
                            spritex += inst->hspeed;
                            spritey += inst->vspeed;
                            setDirection(inst, inst->direction - (20.0 * n));
                        } else if ((int32_t) shake == 43) {
                            setDirection(inst, inst->direction + (30.0 * n));
                            spritex += ((inst->hspeed * 0.7) + 10);
                            spritey += (inst->vspeed * 0.7);
                            setDirection(inst, inst->direction - (30.0 * n));
                        }
                    } else if (!RValue_toBool(callBuiltin(ctx, "instance_exists", (RValue[]){ RValue_makeReal((GMLReal) writerCache.objPapdate) }, 1))) {
                        GMLReal rshake = ((GMLReal) rand() / (GMLReal) RAND_MAX) * shake;
                        GMLReal rshake2 = ((GMLReal) rand() / (GMLReal) RAND_MAX) * shake;
                        spritex += (rshake - (shake / 2.0));
                        spritey += (rshake2 - (shake / 2.0));
                    }
                    GMLReal iconScale = 1.0;
                    if (myfont == writerCache.fntMain || myfont == writerCache.fntJaMain) {
                        iconScale = 2.0;
                    }
                    if (myfont == writerCache.fntMain || myfont == writerCache.fntMaintext) {
                        spritey += (1.0 * iconScale);
                    }
                    if (myfont == writerCache.fntJaPapyrusBtl) {
                        spritex -= 1;
                    }
                    if (myfont == writerCache.fntPapyrus && icontype == 1) {
                        int32_t sprHeight = (sprite >= 0 && dw->sprt.count > (uint32_t) sprite) ? (int32_t) dw->sprt.sprites[sprite].height : 0;
                        spritey += GMLReal_floor((16.0 - sprHeight) / 2.0);
                    }
                    if (vtext) {
                        int32_t sprWidth = (sprite >= 0 && dw->sprt.count > (uint32_t) sprite) ? (int32_t) dw->sprt.sprites[sprite].width : 0;
                        Renderer_drawSpriteExt(renderer, sprite, 0,
                            (float) (spritex - sprWidth), (float) spritey,
                            (float) iconScale, (float) iconScale, 0.0f, 0xFFFFFF, 1.0f);
                        int32_t sprHeight = (sprite >= 0 && dw->sprt.count > (uint32_t) sprite) ? (int32_t) dw->sprt.sprites[sprite].height : 0;
                        myy += ((sprHeight + 1) * iconScale);
                    } else {
                        Renderer_drawSpriteExt(renderer, sprite, 0,
                            (float) spritex, (float) spritey,
                            (float) iconScale, (float) iconScale, 0.0f, 0xFFFFFF, 1.0f);
                        int32_t sprWidth = (sprite >= 0 && dw->sprt.count > (uint32_t) sprite) ? (int32_t) dw->sprt.sprites[sprite].width : 0;
                        myx += ((sprWidth + 1) * iconScale);
                    }
                }
            } else if (ch == '>') {
                n++;
                char choiceCh = nativeStringCharAtBuf(originalstring, n);
                GMLReal choiceindex = 0.0;
                if (choiceCh != '\0') {
                    char buf[2] = { choiceCh, '\0' };
                    choiceindex = GMLReal_strtod(buf, nullptr);
                }
                if ((int32_t) choiceindex == 1) {
                    myx = 196;
                } else {
                    myx = 100;
                    if (myfont == writerCache.fntJaComicsansBig) {
                        myx += 11;
                    }
                }
                // view_wview[view_current]
                int32_t viewCurrent = runner->viewCurrent;
                int32_t viewWview = 0;
                if (viewCurrent >= 0 && 8 > viewCurrent) {
                    viewWview = (int32_t) runner->currentRoom->views[viewCurrent].viewWidth;
                }
                if (viewWview == 640) {
                    myx *= 2;
                }
                // view_xview[view_current]
                int32_t viewXview = 0;
                if (viewCurrent >= 0 && 8 > viewCurrent) {
                    viewXview = (int32_t) runner->currentRoom->views[viewCurrent].viewX;
                }
                myx += viewXview;
            }
        } else if (ch == '&') {
            // Sync myx/myy to instance before SCR_NEWLINE reads them
            Instance_setSelfVar(inst, writerCache.myx, RValue_makeReal(myx));
            Instance_setSelfVar(inst, writerCache.myy, RValue_makeReal(myy));
            // script_execute(SCR_NEWLINE)
            RValue newlineResult = VM_callCodeIndex(ctx, writerCache.scrNewline, nullptr, 0);
            RValue_free(&newlineResult);
            // SCR_NEWLINE modifies myx/myy on the instance, re-read them
            myx = selfReal(inst, writerCache.myx);
            myy = selfReal(inst, writerCache.myy);
        } else if (ch == '/') {
            int32_t halt = 1;
            char nextch = nativeStringCharAtBuf(originalstring, n + 1);
            if (nextch == '%') {
                halt = 2;
            } else if (nextch == '^' && nativeStringCharAtBuf(originalstring, n + 2) != '0') {
                halt = 4;
            } else if (nextch == '*') {
                halt = 6;
            }
            Instance_setSelfVar(inst, writerCache.halt, RValue_makeReal((GMLReal) halt));
            break;
        } else if (ch == '%') {
            if (nativeStringCharAtBuf(originalstring, n + 1) == '%') {
                // instance_destroy()
                Runner_destroyInstance(runner, inst);
                break;
            }
            int32_t stringno = selfInt(inst, writerCache.stringno);
            stringno++;
            Instance_setSelfVar(inst, writerCache.stringno, RValue_makeReal((GMLReal) stringno));

            // originalstring = scr_replace_buttons_pc(mystring[stringno])
            RValue mystringVal = selfArrayGet(inst, writerCache.mystring, stringno);
            RValue replaceArgs[1] = { mystringVal };
            RValue replaceResult = VM_callCodeIndex(ctx, writerCache.scrReplaceButtonsPc, replaceArgs, 1);

            // Free old owned copy
            if (ownedOriginalString != nullptr) {
                free(ownedOriginalString);
                ownedOriginalString = nullptr;
            }

            // Store the result on the instance
            Instance_setSelfVar(inst, writerCache.originalstring, replaceResult);
            // Update our local pointer
            originalstring = selfString(inst, writerCache.originalstring);
            RValue_free(&replaceResult);

            stringpos = 0;
            Instance_setSelfVar(inst, writerCache.stringpos, RValue_makeReal(0.0));
            myx = writingx;
            myy = writingy;
            inst->alarm[0] = selfInt(inst, writerCache.textspeed);
            break;
        } else {
            // Normal character drawing
            char myletter = nativeStringCharAtBuf(originalstring, n);
            if (myletter == '^') {
                n++;
                myletter = nativeStringCharAtBuf(originalstring, n);
            }
            if (!vtext && myx > writingxend) {
                // Sync myx/myy to instance before SCR_NEWLINE reads them
                Instance_setSelfVar(inst, writerCache.myx, RValue_makeReal(myx));
                Instance_setSelfVar(inst, writerCache.myy, RValue_makeReal(myy));
                // script_execute(SCR_NEWLINE)
                RValue newlineResult = VM_callCodeIndex(ctx, writerCache.scrNewline, nullptr, 0);
                RValue_free(&newlineResult);
                myx = selfReal(inst, writerCache.myx);
                myy = selfReal(inst, writerCache.myy);
            }
            GMLReal letterx = myx;
            GMLReal offsetx = 0;
            GMLReal offsety = 0;
            GMLReal halfscale = 1.0;
            if (halfsize) {
                halfscale = 0.5;
                if (vtext) {
                    offsetx += (vspacing * 0.33);
                } else {
                    offsety += (vspacing * 0.33);
                }
            }
            if (isEnglish) {
                if ((int32_t) globalReal(ctx, writerCache.gTyper) == 18) {
                    if (myletter == 'l' || myletter == 'i') letterx += 2;
                    if (myletter == 'I') letterx += 2;
                    if (myletter == '!') letterx += 2;
                    if (myletter == '.') letterx += 2;
                    if (myletter == 'S') letterx += 1;
                    if (myletter == '?') letterx += 2;
                    if (myletter == 'D') letterx += 1;
                    if (myletter == 'A') letterx += 1;
                    if (myletter == '\'') letterx += 1;
                }
            } else if (isJapanese) {
                if (vtext && (myfont == writerCache.fntJaPapyrus || myfont == writerCache.fntJaPapyrusBtl)) {
                    char myletterStr[2] = { myletter, '\0' };
                    bool isBracket = (strcmp(myletterStr, "\xe3\x80\x8c") == 0 || strcmp(myletterStr, "\xe3\x80\x8e") == 0);
                    // Note: This check won't work for multi-byte chars with single char buffer
                    // In practice, these Japanese brackets are multi-byte UTF-8 and won't fit in a single char
                    // The original GML uses string_char_at which handles multi-byte, so this is a simplification
                    if ((int32_t) myy == (int32_t) writingy && isBracket) {
                        // string_width(myletter) - call the builtin
                        RValue swArgs[1] = { RValue_makeString(myletterStr) };
                        RValue sw = callBuiltin(ctx, "string_width", swArgs, 1);
                        myy -= GMLReal_round((RValue_toReal(sw) / 2.0) * htextscale * halfscale);
                        RValue_free(&sw);
                    }
                } else if (myfont == writerCache.fntJaMaintext || myfont == writerCache.fntJaMain) {
                    GMLReal unit = htextscale * halfscale;
                    if (myfont == writerCache.fntJaMain) {
                        unit *= 2;
                    }
                    // Same single-byte limitation as below; kept for GML parity
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
                    int32_t ordVal = (int32_t) (unsigned char) myletter;
                    if (ordVal < 1024 || ordVal == 8211) {
                        if (n > 1) {
                            char lastchChar = nativeStringCharAtBuf(originalstring, n - 1);
                            int32_t lastch = (int32_t) (unsigned char) lastchChar;
                            if (lastch >= 1024 && lastch < 65281 && lastch != 8211 && lastch != 12288) {
                                letterx += unit;
                            }
                        }
#pragma GCC diagnostic pop
                    }
                }
            }

            // scr_setfont(myfont)
            RValue setfontArg = RValue_makeReal((GMLReal) myfont);
            RValue setfontResult = VM_callCodeIndex(ctx, writerCache.scrSetfont, &setfontArg, 1);
            RValue_free(&setfontResult);

            // draw_set_color(mycolor)
            renderer->drawColor = (uint32_t) mycolor;

            GMLReal angle = vtext ? -90.0 : 0.0;

            if (shake > 38) {
                if ((int32_t) shake == 39) {
                    setDirection(inst, inst->direction + 10);
                    offsetx += inst->hspeed;
                    offsety += inst->vspeed;
                } else if ((int32_t) shake == 40) {
                    offsetx += inst->hspeed;
                    offsety += inst->vspeed;
                } else if ((int32_t) shake == 41) {
                    setDirection(inst, inst->direction + (10.0 * n));
                    offsetx += inst->hspeed;
                    offsety += inst->vspeed;
                    setDirection(inst, inst->direction - (10.0 * n));
                } else if ((int32_t) shake == 42) {
                    setDirection(inst, inst->direction + (20.0 * n));
                    offsetx += inst->hspeed;
                    offsety += inst->vspeed;
                    setDirection(inst, inst->direction - (20.0 * n));
                } else if ((int32_t) shake == 43) {
                    setDirection(inst, inst->direction + (30.0 * n));
                    offsetx += ((inst->hspeed * 0.7) + 10);
                    offsety += (inst->vspeed * 0.7);
                    setDirection(inst, inst->direction - (30.0 * n));
                }
            } else {
                GMLReal rshake = ((GMLReal) rand() / (GMLReal) RAND_MAX) * shake;
                GMLReal rshake2 = ((GMLReal) rand() / (GMLReal) RAND_MAX) * shake;
                offsetx += (rshake - (shake / 2.0));
                offsety += (rshake2 - (shake / 2.0));
            }

            // display_scale = surface_get_width(application_surface) / view_wview[view_current]
            GMLReal surfaceWidth = (GMLReal) dw->gen8.defaultWindowWidth;
            int32_t viewCurrent = runner->viewCurrent;
            GMLReal viewWview = 0;
            if (viewCurrent >= 0 && 8 > viewCurrent) {
                viewWview = (GMLReal) runner->currentRoom->views[viewCurrent].viewWidth;
            }
            GMLReal displayScale = (viewWview != 0.0) ? (surfaceWidth / viewWview) : 1.0;

            GMLReal finalx = GMLReal_round((letterx + offsetx) * displayScale) / displayScale;
            GMLReal finaly = GMLReal_round((myy + offsety) * displayScale) / displayScale;

            // draw_text_transformed(finalx, finaly, myletter, htextscale * halfscale, vtextscale * halfscale, angle)
            char letterStr[2] = { myletter, '\0' };
            char* processedText = TextUtils_preprocessGmlTextIfNeeded(runner, letterStr);
            renderer->vtable->drawText(renderer, processedText, (float) finalx, (float) finaly,
                (float) (htextscale * halfscale), (float) (vtextscale * halfscale), (float) angle);
            free(processedText);

            letterx += spacing;

            if (isEnglish) {
                if (myfont == writerCache.fntComicsans) {
                    if (myletter == 'w') letterx += 2;
                    if (myletter == 'm') letterx += 2;
                    if (myletter == 'i') letterx -= 2;
                    if (myletter == 'l') letterx -= 2;
                    if (myletter == 's') letterx -= 1;
                    if (myletter == 'j') letterx -= 1;
                } else if (myfont == writerCache.fntPapyrus) {
                    if (myletter == 'D') letterx += 1;
                    if (myletter == 'Q') letterx += 3;
                    if (myletter == 'M') letterx += 1;
                    if (myletter == 'L') letterx -= 1;
                    if (myletter == 'K') letterx -= 1;
                    if (myletter == 'C') letterx += 1;
                    if (myletter == '.') letterx -= 3;
                    if (myletter == '!') letterx -= 3;
                    if (myletter == 'O' || myletter == 'W') letterx += 2;
                    if (myletter == 'I') letterx -= 6;
                    if (myletter == 'T') letterx -= 1;
                    if (myletter == 'P') letterx -= 2;
                    if (myletter == 'R') letterx -= 2;
                    if (myletter == 'A') letterx += 1;
                    if (myletter == 'H') letterx += 1;
                    if (myletter == 'B') letterx += 1;
                    if (myletter == 'G') letterx += 1;
                    if (myletter == 'F') letterx -= 1;
                    if (myletter == '?') letterx -= 3;
                    if (myletter == '\'') letterx -= 6;
                    if (myletter == 'J') letterx -= 1;
                }
            } else if (isJapanese) {
                // Note: Japanese text support requires proper UTF-8 multi-byte character handling.
                // The single-byte char approach here only handles ASCII correctly. For multi-byte
                // characters (ord >= 256), the GML original uses string_char_at which returns full
                // Unicode codepoints. For now, single-byte characters always fall through to the
                // "< 1024" branch since unsigned char maxes at 255.
                // ordVal will only be 0-255 for single-byte chars; the >= 65377 / == 8211 branches
                // are unreachable but kept for parity with the GML original (future UTF-8 support)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
                int32_t ordVal = (int32_t) (unsigned char) myletter;
                if (vtext) {
                    // string_width(myletter)
                    RValue swArgs[1] = { RValue_makeString(letterStr) };
                    RValue sw = callBuiltin(ctx, "string_width", swArgs, 1);
                    myy += GMLReal_round(RValue_toReal(sw) * htextscale * halfscale);
                    RValue_free(&sw);
                } else if (myletter == ' ' || ordVal >= 65377) {
                    letterx -= GMLReal_floor(spacing / 2.0);
                } else if (1024 > ordVal || ordVal == 8211) {
                    if (myfont == writerCache.fntJaComicsans || myfont == writerCache.fntJaComicsansBig) {
                        letterx -= GMLReal_floor(spacing * 0.3);
                    } else {
                        letterx -= GMLReal_floor(spacing * 0.4);
                    }
                }
#pragma GCC diagnostic pop
            }

            if (!vtext) {
                if (halfsize) {
                    myx = GMLReal_round(myx + ((letterx - myx) / 2.0));
                } else {
                    myx = letterx;
                }
            }
        }
    }

    // Write back myx, myy and mycolor to the instance
    Instance_setSelfVar(inst, writerCache.myx, RValue_makeReal(myx));
    Instance_setSelfVar(inst, writerCache.myy, RValue_makeReal(myy));
    Instance_setSelfVar(inst, writerCache.mycolor, RValue_makeReal((GMLReal) mycolor));

    if (ownedOriginalString != nullptr) {
        free(ownedOriginalString);
    }
}

// ===[ Initialization ]===

void NativeScripts_init(VMContext* ctx, [[maybe_unused]] Runner* runner) {
    DataWin* dw = ctx->dataWin;

    // Initialize caches
    initWriterCache(ctx, dw);

    // Register native overrides
    registerNative("gml_Object_obj_base_writer_Draw_0", native_objBaseWriter_Draw0);

    fprintf(stderr, "NativeScripts: Registered %d native code overrides\n", (int32_t) shlen(nativeOverrideMap));
}
