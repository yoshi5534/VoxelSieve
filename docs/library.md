# Projects, operations and plugins

The library records work as a project: a directory with `project.json` and one directory per step.
Each step stores its operation, parameters, inputs, outputs and messages; undo and redo move a
cursor over the steps, and a new step after an undo discards the undone ones once it is done (a
step that fails keeps them for redo). Operations never change their inputs. A project is open in
one program at a time; a second one is refused ([ADR 0036](adr/0036-project-lock.md)). An import that was interrupted, by a
full disk or the end of the program, keeps its files and continues from its journal when it is
run again with the same parameters ([ADR 0037](adr/0037-steps-that-resume.md)).

```cpp
voxelsieve::OperationRegistry registry;
voxelsieve::registerBuiltinOperations(registry);   // open_dataset, import_raw, import_tiff, porosity, …
registry.loadPlugins("plugins");                    // every *.so (*.dll) in the directory
auto project = voxelsieve::Project::create("casting.vsproj", "Casting 4711");
project.run(registry, "import_raw", {{"path", "scan.raw"}});
project.run(registry, "porosity");                  // input: the latest dataset
project.run(registry, "report", {{"order_path", "examples/inspection_order.json"}});
project.undo();
```

A plugin is a shared library that exports `voxelsieve_plugin_api_version()`,
`voxelsieve_plugin_build()` and `voxelsieve_register_operations()`;
`examples/plugins/histogram` is a complete one. The release packages hold the headers and a CMake
package, so a plugin builds against an installed VoxelSieve:

```cmake
find_package(VoxelSieve REQUIRED)        # with -DCMAKE_PREFIX_PATH=<unpacked package>
add_library(my_plugin MODULE my_plugin.cpp)
target_link_libraries(my_plugin PRIVATE VoxelSieve::voxelsieve)
```

The interface is C++, so the plugin needs the standard library, OpenVDB and nlohmann/json of the
VoxelSieve that loads it. `voxelsieve_plugin_build()` returns `VOXELSIEVE_PLUGIN_BUILD`, which
names them; a plugin whose text differs is not loaded, and the message shows both. See
[ADR 0008](adr/0008-gui-projects-operations-ui.md).
