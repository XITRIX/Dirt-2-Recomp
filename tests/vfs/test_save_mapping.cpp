#include "shims/vfs.h"
#include <cassert>
#include <string>
namespace rt { std::string game_dir() { return "/game"; } }
int main() {
    const std::string win = "C:\\Users\\Player\\Documents\\My Games\\DiRT2\\";
    const std::string data = "/prototype";
    const std::string original = "/original/savegame";
    vfs::set_data_dir(data);
    assert(vfs::host_path(win+"savegame/profile/save.dat") == data+"/C/Users/Player/Documents/My Games/DiRT2/savegame/profile/save.dat");
    vfs::set_save_dir(original+"/");
    assert(vfs::save_dir() == original);
    assert(vfs::host_path(win+"savegame") == original);
    assert(vfs::host_path(win+"SAVEGAME/profile/save.dat") == original+"/profile/save.dat");
    assert(vfs::host_path(win+"savegame/../hardwaresettings/config.xml") == data+"/C/Users/Player/Documents/My Games/DiRT2/hardwaresettings/config.xml");
    assert(vfs::host_path(win+"savegame-old/a") == data+"/C/Users/Player/Documents/My Games/DiRT2/savegame-old/a");
    assert(vfs::host_path("D:"+win.substr(2)+"savegame/a").starts_with(data+"/D/"));
    assert(vfs::is_data_path(original+"/new-profile/save.dat"));
    assert(!vfs::is_data_path(original+"-other/save.dat"));
    assert(!vfs::is_data_path(data+"-other/file"));
    vfs::set_override("system/test.bin", "/patched");
    assert(vfs::host_path("C:/Dirt2/system/test.bin") == "/patched");
    vfs::set_save_dir("");
    assert(vfs::host_path(win+"savegame/a").starts_with(data+"/C/"));
}
