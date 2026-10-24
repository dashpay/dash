GUI changes
-----------

- The Window menu has new Zoom In, Zoom Out and Reset Zoom entries that change
  the font scale of the whole application, bound to the platform zoom
  shortcuts (Ctrl++ / Ctrl+= and Ctrl+- / Ctrl+_; Cmd on macOS) and Ctrl+0.
  The new scale is saved like the Appearance tab's font scale setting. Like
  that slider, the entries are disabled when `-font-scale` is given on the
  command line. Inside the RPC console window the zoom-in/out keys still resize
  only the console text, and when the console is the main window
  (`-disablewallet` or builds without wallet support) the keys stay with the
  console while the menu entries keep zooming the whole application. (#7216)

- The font scale range is now -50 to 100, both for `-font-scale` and the
  Appearance tab slider, which used to stop at -30 and 30. `-font-scale` values
  below -50 on the command line or in the configuration file, which earlier
  versions accepted down to -100 although they shrank text towards zero size,
  are now rejected at startup. A font scale saved in `settings.json`
  outside the range is clamped on load and the clamped value is saved. (#7216)
