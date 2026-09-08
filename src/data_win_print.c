#include "data_win.h"

#include "stdio_compat.h"
#include "string_compat.h"

#include "utils.h"

void DataWin_printDebugSummary(DataWin* dataWin) {
    logInfo("===== data.win Summary =====\n\n");

    // GEN8
    Gen8* g = &dataWin->gen8;
    logInfo("-- GEN8 (General Info) --\n");
    logInfo("  Game Name:        %s\n", g->name ? g->name : "(null)");
    logInfo("  Display Name:     %s\n", g->displayName ? g->displayName : "(null)");
    logInfo("  File Name:        %s\n", g->fileName ? g->fileName : "(null)");
    logInfo("  Config:           %s\n", g->config ? g->config : "(null)");
    logInfo("  WAD Version: %u\n", g->wadVersion);
    logInfo("  Game ID:          %u\n", g->gameID);
    logInfo("  Version:          %u.%u.%u.%u\n", g->major, g->minor, g->release, g->build);
    logInfo("  Window Size:      %ux%u\n", g->defaultWindowWidth, g->defaultWindowHeight);
    logInfo("  Steam App ID:     %d\n", g->steamAppID);
    logInfo("  Room Order:       %u rooms\n", g->roomOrderCount);
    logInfo("\n");

    // OPTN
    logInfo("-- OPTN (Options) --\n");
    logInfo("  Constants:        %u\n", dataWin->optn.constantCount);
    if (dataWin->optn.constantCount > 0) {
        uint32_t show = dataWin->optn.constantCount < 3 ? dataWin->optn.constantCount : 3;
        forEachIndexed(OptnConstant, constant, idx, dataWin->optn.constants, show) {
            logInfo("    [%u] %s = %s\n", (unsigned int)idx, constant->name ? constant->name : "?", constant->value ? constant->value : "?");
        }
        if (dataWin->optn.constantCount > 3) logInfo("    ... and %u more\n", dataWin->optn.constantCount - 3);
    }
    logInfo("\n");

    // LANG
    logInfo("-- LANG (Languages) --\n");
    logInfo("  Languages:        %u\n", dataWin->lang.languageCount);
    logInfo("  Entries:          %u\n", dataWin->lang.entryCount);
    logInfo("\n");

    // EXTN
    logInfo("-- EXTN (Extensions) --\n");
    logInfo("  Extensions:       %u\n", dataWin->extn.count);
    forEachIndexed(Extension, ext, idx, dataWin->extn.extensions, dataWin->extn.count) {
        logInfo("    [%u] %s (%u files)\n", (unsigned int)idx, ext->name ? ext->name : "?", ext->fileCount);
    }
    logInfo("\n");

    // SOND
    logInfo("-- SOND (Sounds) --\n");
    logInfo("  Sounds:           %u\n", dataWin->sond.count);
    if (dataWin->sond.count > 0) {
        uint32_t show = dataWin->sond.count < 3 ? dataWin->sond.count : 3;
        forEachIndexed(Sound, snd, idx, dataWin->sond.sounds, show) {
            logInfo("    [%u] %s (%s)\n", (unsigned int)idx, snd->name ? snd->name : "?", snd->type ? snd->type : "?");
        }
        if (dataWin->sond.count > 3) logInfo("    ... and %u more\n", dataWin->sond.count - 3);
    }
    logInfo("\n");

    // AGRP
    logInfo("-- AGRP (Audio Groups) --\n");
    logInfo("  Audio Groups:     %u\n", dataWin->agrp.count);
    {
    forEachIndexed(AudioGroup, ag, idx, dataWin->agrp.audioGroups, dataWin->agrp.count) {
        logInfo("    [%u] %s\n", (unsigned int)idx, ag->name ? ag->name : "?");
    }
    }
    logInfo("\n");

    // SPRT
    logInfo("-- SPRT (Sprites) --\n");
    logInfo("  Sprites:          %u\n", dataWin->sprt.count);
    if (dataWin->sprt.count > 0) {
        uint32_t show = dataWin->sprt.count < 3 ? dataWin->sprt.count : 3;
        forEachIndexed(Sprite, spr, idx, dataWin->sprt.sprites, show) {
            logInfo("    [%u] %s (%ux%u, %u frames)\n", (unsigned int)idx, spr->name ? spr->name : "?", spr->width, spr->height, spr->textureCount);
        }
        if (dataWin->sprt.count > 3) logInfo("    ... and %u more\n", dataWin->sprt.count - 3);
    }
    logInfo("\n");

    // BGND
    logInfo("-- BGND (Backgrounds) --\n");
    logInfo("  Backgrounds:      %u\n", dataWin->bgnd.count);
    if (dataWin->bgnd.count > 0) {
        uint32_t show = dataWin->bgnd.count < 3 ? dataWin->bgnd.count : 3;
        forEachIndexed(Background, bg, idx, dataWin->bgnd.backgrounds, show) {
            logInfo("    [%u] %s\n", (unsigned int)idx, bg->name ? bg->name : "?");
        }
        if (dataWin->bgnd.count > 3) logInfo("    ... and %u more\n", dataWin->bgnd.count - 3);
    }
    logInfo("\n");

    // PATH
    logInfo("-- PATH (Paths) --\n");
    logInfo("  Paths:            %u\n", dataWin->path.count);
    logInfo("\n");

    // SCPT
    logInfo("-- SCPT (Scripts) --\n");
    logInfo("  Scripts:          %u\n", dataWin->scpt.count);
    if (dataWin->scpt.count > 0) {
        uint32_t show = dataWin->scpt.count < 3 ? dataWin->scpt.count : 3;
        forEachIndexed(Script, scr, idx, dataWin->scpt.scripts, show) {
            logInfo("    [%u] %s -> code[%d]\n", (unsigned int)idx, scr->name ? scr->name : "?", scr->codeId);
        }
        if (dataWin->scpt.count > 3) logInfo("    ... and %u more\n", dataWin->scpt.count - 3);
    }
    logInfo("\n");

    // GLOB
    logInfo("-- GLOB (Global Init Scripts) --\n");
    logInfo("  Init Scripts:     %u\n", dataWin->glob.count);
    logInfo("\n");

    // SHDR
    logInfo("-- SHDR (Shaders) --\n");
    logInfo("  Shaders:          %u\n", dataWin->shdr.count);
    {
    forEachIndexed(Shader, shdr, idx, dataWin->shdr.shaders, dataWin->shdr.count) {
        logInfo("    [%u] %s (version %d)\n", (unsigned int)idx, shdr->name ? shdr->name : "?", shdr->version);
    }
    }
    logInfo("\n");

    // FONT
    logInfo("-- FONT (Fonts) --\n");
    logInfo("  Fonts:            %u\n", dataWin->font.count);
    {
    forEachIndexed(Font, fnt, idx, dataWin->font.fonts, dataWin->font.count) {
        logInfo("    [%u] %s (%s, em=%g, %u glyphs)\n", (unsigned int)idx, fnt->name ? fnt->name : "?", fnt->displayName ? fnt->displayName : "?", (double)fnt->emSize, fnt->glyphCount);
    }
    }
    logInfo("\n");

    // TMLN
    logInfo("-- TMLN (Timelines) --\n");
    logInfo("  Timelines:        %u\n", dataWin->tmln.count);
    logInfo("\n");

    // OBJT
    logInfo("-- OBJT (Game Objects) --\n");
    logInfo("  Objects:          %u\n", dataWin->objt.count);
    if (dataWin->objt.count > 0) {
        uint32_t show = dataWin->objt.count < 3 ? dataWin->objt.count : 3;
        forEachIndexed(GameObject, obj, idx, dataWin->objt.objects, show) {
            uint32_t totalEvents = 0;
            repeat(OBJT_EVENT_TYPE_COUNT, e) {
                totalEvents += obj->eventLists[e].eventCount;
            }
            logInfo("    [%u] %s (sprite=%d, depth=%d, %u events)\n", (unsigned int)idx, obj->name ? obj->name : "?", obj->spriteId, obj->depth, totalEvents);
        }
        if (dataWin->objt.count > 3) logInfo("    ... and %u more\n", dataWin->objt.count - 3);
    }
    logInfo("\n");

    // ROOM
    logInfo("-- ROOM (Rooms) --\n");
    logInfo("  Rooms:            %u\n", dataWin->room.count);
    if (dataWin->room.count > 0) {
        uint32_t show = dataWin->room.count < 3 ? dataWin->room.count : 3;
        forEachIndexed(Room, rm, idx, dataWin->room.rooms, show) {
            if (rm->payloadLoaded) {
                logInfo("    [%u] %s (%ux%u, %u objects, %u tiles)\n", (unsigned int)idx, rm->name ? rm->name : "?", rm->width, rm->height, rm->gameObjectCount, rm->tileCount);
            } else {
                // Lazy room with payload not yet loaded: gameObjectCount/tileCount would be 0 and misleading.
                logInfo("    [%u] %s (%ux%u, payload not loaded)\n", (unsigned int)idx, rm->name ? rm->name : "?", rm->width, rm->height);
            }
        }
        if (dataWin->room.count > 3) logInfo("    ... and %u more\n", dataWin->room.count - 3);
    }
    logInfo("\n");

    // TPAG
    logInfo("-- TPAG (Texture Page Items) --\n");
    logInfo("  Items:            %u\n", dataWin->tpag.count);
    logInfo("\n");

    // CODE
    logInfo("-- CODE (Code Entries) --\n");
    logInfo("  Entries:          %u\n", dataWin->code.count);
    if (dataWin->code.count > 0) {
        uint32_t show = dataWin->code.count < 3 ? dataWin->code.count : 3;
        forEachIndexed(CodeEntry, entry, idx, dataWin->code.entries, show) {
            logInfo("    [%u] %s (%u bytes, %u locals, %u args)\n", (unsigned int)idx, entry->name ? entry->name : "?", entry->length, entry->localsCount, entry->argumentsCount);
        }
        if (dataWin->code.count > 3) logInfo("    ... and %u more\n", dataWin->code.count - 3);
    }
    logInfo("\n");

    // VARI
    logInfo("-- VARI (Variables) --\n");
    logInfo("  Variables:        %u\n", dataWin->vari.variableCount);
    logInfo("  Max Locals:       %u\n", dataWin->vari.maxLocalVarCount);
    if (dataWin->vari.variableCount > 0) {
        uint32_t show = dataWin->vari.variableCount < 3 ? dataWin->vari.variableCount : 3;
        forEachIndexed(Variable, var, idx, dataWin->vari.variables, show) {
            logInfo("    [%u] %s (type=%d, id=%d, %u refs)\n", (unsigned int)idx, var->name ? var->name : "?", var->instanceType, var->varID, var->occurrences);
        }
        if (dataWin->vari.variableCount > 3) logInfo("    ... and %u more\n", dataWin->vari.variableCount - 3);
    }
    logInfo("\n");

    // FUNC
    logInfo("-- FUNC (Functions) --\n");
    logInfo("  Functions:        %u\n", dataWin->func.functionCount);
    logInfo("  Code Locals:      %u\n", dataWin->func.codeLocalsCount);
    if (dataWin->func.functionCount > 0) {
        uint32_t show = dataWin->func.functionCount < 3 ? dataWin->func.functionCount : 3;
        forEachIndexed(Function, fn, idx, dataWin->func.functions, show) {
            logInfo("    [%u] %s (%u refs)\n", (unsigned int)idx, fn->name ? fn->name : "?", fn->occurrences);
        }
        if (dataWin->func.functionCount > 3) logInfo("    ... and %u more\n", dataWin->func.functionCount - 3);
    }
    logInfo("\n");

    // STRG
    logInfo("-- STRG (Strings) --\n");
    logInfo("  Strings:          %u\n", dataWin->strg.count);
    if (dataWin->strg.count > 0) {
        uint32_t show = dataWin->strg.count < 5 ? dataWin->strg.count : 5;
        repeat(show, i) {
            const char* str = dataWin->strg.strings[i];
            // Truncate long strings for display
            if (str) {
                size_t len = strlen(str);
                if (len > 60) {
                    logInfo("    [%u] \"%.60s...\" (%zu chars)\n", (unsigned int)i, str, len);
                } else {
                    logInfo("    [%u] \"%s\"\n", (unsigned int)i, str);
                }
            } else {
                logInfo("    [%u] (null)\n", (unsigned int)i);
            }
        }
        if (dataWin->strg.count > 5) logInfo("    ... and %u more\n", dataWin->strg.count - 5);
    }
    logInfo("\n");

    // TXTR
    logInfo("-- TXTR (Textures) --\n");
    logInfo("  Textures:         %u\n", dataWin->txtr.count);
    if (dataWin->txtr.count > 0) {
        forEachIndexed(Texture, tex, idx, dataWin->txtr.textures, dataWin->txtr.count) {
            logInfo("    [%u] offset=0x%08X size=%u bytes\n", (unsigned int)idx, tex->blobOffset, tex->blobSize);
        }
    }
    logInfo("\n");

    // AUDO
    logInfo("-- AUDO (Audio) --\n");
    logInfo("  Audio Entries:    %u\n", dataWin->audo.count);
    if (dataWin->audo.count > 0) {
        uint32_t show = dataWin->audo.count < 3 ? dataWin->audo.count : 3;
        forEachIndexed(AudioEntry, ae, idx, dataWin->audo.entries, show) {
            logInfo("    [%u] offset=0x%08X size=%u bytes\n", (unsigned int)idx, ae->dataOffset, ae->dataSize);
        }
        if (dataWin->audo.count > 3) logInfo("    ... and %u more\n", dataWin->audo.count - 3);
    }
    logInfo("\n");

    logInfo("-- Room Instances --\n");
    forEach(Room, room, dataWin->room.rooms, dataWin->room.count) {
        logInfo("Room %s\n", room->name);

        if (!room->payloadLoaded) {
            logInfo("  (payload not loaded)\n");
            continue;
        }

        forEachIndexed(RoomGameObject, roomGameObject, idx, room->gameObjects, room->gameObjectCount) {
            int32_t objectDefinitionId = roomGameObject->objectDefinition;
            GameObject* objectDefinition = &dataWin->objt.objects[objectDefinitionId];
            logInfo("  Object %d (%s, x=%d, y=%d)\n", objectDefinitionId, objectDefinition->name, roomGameObject->x, roomGameObject->y);
        }
    }

    // Overall summary
    logInfo("===== DataWin parse complete =====\n");
}
