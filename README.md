![GlooPrint - Press F. Tidy your Blueprints.](assets/glooprint-fab-thumbnail.jpg)

Blueprints get messy pretty quickly. GlooPrint helps with that - press **F** and it arranges the graph for you.

Select a node first if you'd like it to stay where it is. The whole active graph gets formatted around it, including groups that aren't connected. Your camera, zoom and selection stay put, so you can carry on from where you were.

Pick rounded wires, 45-degree turns or Unreal's own splines. You still get the usual pin colours, hover feedback and execution highlights. Double-click a visible wire to add a normal reroute. Your connections, pin values and existing reroutes are kept, and a changed layout can be undone in one step.

Download GlooPrint from [GitHub Releases](https://github.com/ishtms/glooprint/releases). For Windows with UE 5.8.2, choose the `Win64.zip` asset, close the editor, and extract its `GlooPrint` folder into your project's `Plugins` directory. Enable it under **Edit > Plugins** and restart if prompted. Open a Blueprint graph, select a node, put the pointer over the canvas and press **F**. You can also use **GlooPrint > Format Graph** in the graph's context menu or change the shortcut in Editor Preferences > Keyboard Shortcuts.

Wire and spacing settings are under **Editor Preferences > Plugins > GlooPrint**. It starts with rounded wires, 96 horizontal spacing, 48 vertical spacing and 32 comment padding. Settings are saved for you in the current project.

Event Graphs, functions, macros, Construction Scripts, opened collapsed graphs and graphs made entirely of pure nodes are supported. An Animation Blueprint's ordinary Event Graph works too. Material graphs, animation pose graphs, Niagara, Control Rig and behaviour trees aren't supported.

Very large graphs, or nodes with lots of pins, can take minutes on the first pass and may pause the editor. Some connections use Unreal's usual curves while GlooPrint works out a route. Other formatting and wire plugins can clash with the shortcut or drawing. The [user guide](Documentation/UserGuide.txt) covers installation from source, settings, gestures and things to check if something goes wrong.

For macOS or a source build, download the `Source.zip` asset from [GitHub Releases](https://github.com/ishtms/glooprint/releases), place its `GlooPrint` folder in your project's `Plugins` directory, and compile with your UE 5.8 engine's C++ toolchain (Xcode on Mac or Visual Studio on Windows). The Windows binary requires the matching UE 5.8.2 engine build; the source ZIP includes no prebuilt binaries. The [user guide](Documentation/UserGuide.txt) has more installation details.

The C++ source and guide are included. There are no extra plugins or services to sign up for. Turn GlooPrint off later and your Blueprints still work, with the saved layout and Unreal's usual wires. The cover combines artwork and actual Unreal captures. The gallery uses native graph captures with captions.

[Get help or report a bug](https://github.com/ishtms/glooprint/issues). Include your engine and plugin versions, OS and a small example that shows what's going wrong.
