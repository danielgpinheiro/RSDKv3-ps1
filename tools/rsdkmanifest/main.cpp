/*
 * rsdkmanifest — host-side sprite manifest for the RSDKv3 (Sonic CD) PS1 port (docs/28 phase 1).
 *
 * Sonic CD ships its scripts as bytecode only, so the frames a stage can draw come from running
 * them: for every stage of the four GameConfig stage lists this tool loads the global + stage
 * bytecode exactly as LoadStageFiles does (RSDKv3/Scene.cpp), runs every object's startup sub
 * with the engine's own ProcessStartupObjects (RSDKv3/Object.cpp; SpriteFrame records frames
 * only there), and writes what the console will need in VRAM:
 *
 *   <out>/<folder>.txt   (list positions sharing a folder append their own `stage` block)
 *     stage <list> <position> <folder> <id>
 *     sheet <sheetID> <path under Data/Sprites/>
 *     frame <sheetID> <x> <y> <w> <h> obj <object name>          script frames (drawn with the
 *                                                                 object's final sheet)
 *     frame <sheetID> <x> <y> <w> <h> ani <file>                  animation (.ani) frames
 *
 * Sheets are only registered (host_stubs.cpp AddGraphicsFile), never decoded here.
 * Usage: rsdkmanifest <root containing Data/> <out dir>
 */
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "RetroEngine.hpp"

static std::string readStr() {
    byte len = 0;
    FileRead(&len, 1);
    char buf[0x100];
    FileRead(buf, len);
    buf[len] = 0;
    return buf;
}

static int globalObjectCount = 0;
static std::string globalNames[OBJECT_COUNT];

static bool loadGameConfig() {
    FileInfo info;
    if (!LoadFile("Data/Game/GameConfig.bin", &info))
        return false;
    readStr(), readStr(), readStr(); // window text, data folder, description
    byte n = 0;
    FileRead(&n, 1);
    globalObjectCount = n;
    for (int i = 0; i < n; ++i) globalNames[i] = readStr();
    for (int i = 0; i < n; ++i) readStr(); // script paths (bytecode used instead)
    FileRead(&n, 1);
    for (int i = 0; i < n; ++i) {
        readStr();
        byte v[4];
        FileRead(v, 4);
    }
    FileRead(&n, 1);
    for (int i = 0; i < n; ++i) readStr(); // sfx
    FileRead(&n, 1);
    for (int i = 0; i < n; ++i) readStr(); // players
    for (int c = 0; c < 4; ++c) {
        int cat = c == 2 ? 3 : c == 3 ? 2 : c; // same list order swap as LoadGameConfig
        byte count = 0;
        FileRead(&count, 1);
        stageListCount[cat] = count;
        for (int s = 0; s < count; ++s) {
            StrCopy(stageList[cat][s].folder, readStr().c_str());
            StrCopy(stageList[cat][s].id, readStr().c_str());
            StrCopy(stageList[cat][s].name, readStr().c_str());
            byte hl = 0;
            FileRead(&hl, 1);
            stageList[cat][s].highlighted = hl;
        }
    }
    CloseFile();
    return true;
}

// The script part of LoadStageFiles (RSDKv3/Scene.cpp), bytecode mode.
static int loadStageScripts() {
    ClearScriptData();
    for (int i = SURFACE_COUNT; i > 0; i--) StrCopy(gfxSurface[i - 1].fileName, "");
    int scriptID = 1;
    FileInfo info;
    bool loadGlobals = false;
    if (LoadStageFile("StageConfig.bin", stageListPosition, &info)) {
        byte b = 0;
        FileRead(&b, 1);
        loadGlobals = b;
        CloseFile();
    }
    if (loadGlobals) {
        for (int i = 0; i < globalObjectCount; ++i) SetObjectTypeName(globalNames[i].c_str(), scriptID + i);
        LoadBytecode(4, scriptID);
        scriptID += globalObjectCount;
    }
    int stageObjects = 0;
    if (LoadStageFile("StageConfig.bin", stageListPosition, &info)) {
        byte b = 0;
        FileRead(&b, 1);
        byte pal[32 * 3];
        FileRead(pal, sizeof(pal));
        byte n = 0;
        FileRead(&n, 1);
        stageObjects = n;
        for (int i = 0; i < n; ++i) SetObjectTypeName(readStr().c_str(), scriptID + i);
        CloseFile();
        LoadBytecode(activeStageList, scriptID);
    }
    return scriptID + stageObjects;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: rsdkmanifest <root containing Data/> <out dir>\n");
        return 1;
    }
    std::string out = argv[2];
    char cwd[0x400];
    if (out[0] != '/' && getcwd(cwd, sizeof(cwd)))
        out = std::string(cwd) + "/" + out;
    mkdir(out.c_str(), 0755);
    if (chdir(argv[1])) {
        perror(argv[1]);
        return 1;
    }
    activePalette   = fullPalette[0]; // the engine points these at palette bank 0 at init
    activePalette32 = fullPalette32[0];
    Engine.usingBytecode = true;
    Engine.bytecodeMode  = BYTECODE_PC; // Steam Sonic CD: GS000.bin + <P|R|B|S>S<nnn>.bin
    if (!loadGameConfig()) {
        fprintf(stderr, "no Data/Game/GameConfig.bin\n");
        return 1;
    }
    int stages = 0;
    std::set<std::string> written; // several list positions can share a folder: append (union)
    for (int list = 0; list < STAGELIST_MAX; ++list) {
        for (int pos = 0; pos < stageListCount[list]; ++pos) {
            activeStageList   = list;
            stageListPosition = pos;
            int objects       = loadStageScripts();
            ProcessStartupObjects();
            std::string path = out + "/" + stageList[list][pos].folder + ".txt";
            FILE *f          = fopen(path.c_str(), written.count(path) ? "a" : "w");
            written.insert(path);
            fprintf(f, "stage %d %d %s %s\n", list, pos, stageList[list][pos].folder, stageList[list][pos].id);
            for (int s = 0; s < SURFACE_COUNT; ++s)
                if (gfxSurface[s].fileName[0])
                    fprintf(f, "sheet %d %s\n", s, gfxSurface[s].fileName + 13); // strip "Data/Sprites/"
            int frames = 0;
            for (int o = 0; o < objects && o < OBJECT_COUNT; ++o) {
                ObjectScript *os = &objectScriptList[o];
                for (int i = 0; i < os->frameCount; ++i) {
                    SpriteFrame *fr = &scriptFrames[os->frameListOffset + i];
                    fprintf(f, "frame %d %d %d %d %d obj %s\n", os->spriteSheetID, fr->sprX, fr->sprY, fr->width, fr->height,
                            typeNames[o]);
                    ++frames;
                }
            }
            for (int a = 0; a < animationFileCount; ++a) {
                AnimationFile *af = &animationFileList[a];
                for (int an = 0; an < af->animCount; ++an) {
                    SpriteAnimation *anim = &animationList[af->aniListOffset + an];
                    for (int i = 0; i < anim->frameCount; ++i) {
                        SpriteFrame *fr = &animFrames[anim->frameListOffset + i];
                        fprintf(f, "frame %d %d %d %d %d ani %s\n", fr->sheetID, fr->sprX, fr->sprY, fr->width, fr->height,
                                af->fileName);
                        ++frames;
                    }
                }
            }
            fclose(f);
            printf("%-5s %-8s %3d objects %4d frames\n", list == 0 ? "pres" : list == 1 ? "reg" : list == 2 ? "bonus" : "spec",
                   stageList[list][pos].folder, objects - 1, frames);
            ++stages;
        }
    }
    printf("%d stages\n", stages);
    return 0;
}
