# EDF5 ModLoader

A very basic rudimentary modloader for Earth Defence Force 5.  
Supports automatic Root.cpk redirection and DLL plugin loading.  
Writes internal game logging to game.log

This repository contains submodules! Please use `--recurse-submodules` when cloning.

## Installation
Get the latest package from [Releases](https://github.com/BlueAmulet/EDF5ModLoader/releases) and unpack it in the same folder as EDF5.exe

https://github.com/BlueAmulet/EDF5ModLoader/releases

## Plugins
### Patcher
Patcher is a plugin to perform runtime memory patches. It accepts .txt files in `Mods\Patches` of the format:  
```Offset: Hex bytes ; Optional comment```  
Where 'Offset' is a hexadecimal offset in memory from the base address of EDF5.exe  
And 'Hex bytes' are a series of hexadecimal bytes to patch into that address.  
All data including and following a semicolon is ignored up to the end of the line.  
Patches by [Souzooka](https://github.com/Souzooka) are included by default.  

### Making your own
The Plugin API is in `PluginAPI.h` and is currently unfinished and subject to change.  
Plugins should export a function of type `bool __fastcall EML5_Load(PluginInfo*)`  
Return true to remain loaded in memory, and false to unload in case of error or desired behavior.  
If your plugin remains in memory, fill out the PluginInfo struct:  
```
pluginInfo->infoVersion = PluginInfo::MaxInfoVer;
pluginInfo->name = "Plugin Name";
pluginInfo->version = PLUG_VER(Major, Minor, Patch, Build);
```

## Building
You will need [Visual Studio 2019](https://visualstudio.microsoft.com/vs/community/) and [vcpkg](https://github.com/microsoft/vcpkg)

To setup vcpkg and required libraries:  
```
git clone https://github.com/microsoft/vcpkg
cd vcpkg
bootstrap-vcpkg.bat
vcpkg install zydis:x64-windows-static-md plog:x64-windows-static-md
```