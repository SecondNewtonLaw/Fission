//
// Minimal Luau bytecode runner used as an execution oracle for test samples.
//

#include "lua.h"
#include "lualib.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <file.luac>\n";
        return 1;
    }

    std::ifstream stream(argv[1], std::ios::binary);
    if (!stream) {
        std::cerr << "error: could not open '" << argv[1] << "'\n";
        return 1;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string bytecode = buffer.str();

    lua_State *state = luaL_newstate();
    if (!state) {
        std::cerr << "error: could not create Luau state\n";
        return 1;
    }
    luaL_openlibs(state);

    if (luau_load(state, argv[1], bytecode.data(), bytecode.size(), 0) != 0) {
        const char *message = lua_tostring(state, -1);
        std::cerr << "load error: " << (message ? message : "unknown") << "\n";
        lua_close(state);
        return 1;
    }

    if (lua_pcall(state, 0, 0, 0) != 0) {
        const char *message = lua_tostring(state, -1);
        std::cerr << "runtime error: " << (message ? message : "unknown") << "\n";
        lua_close(state);
        return 1;
    }

    lua_close(state);
    return 0;
}
