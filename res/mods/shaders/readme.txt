Egg of Time - shader mods
=========================

Put RetroArch shaders in this folder to draw the game through them: CRT screens,
LCD grids, smoothing and scaling filters. Pick one in System Settings > Shader (the
setting is listed once this folder has shaders). Shader replaces the Graphics filter
and Internal resolution settings while it is on.

    glsl\     GLSL shaders: presets (.glslp), and single shaders (.glsl)
    slang\    Slang shaders: presets (.slangp), and single shaders (.slang)

Presets are listed from any subfolder; single shaders only from glsl\ or slang\
itself. So RetroArch's shader collections can be copied in whole, keeping their
folders: the shader files their presets use (and the textures) stay where the
presets expect them. For example:

    https://github.com/libretro/glsl-shaders    -> contents into glsl\
    https://github.com/libretro/slang-shaders   -> contents into slang\

The list shows each shader by its file name; the bottom line of the menu shows the
chosen one's folder. Shaders are found when the game starts: restart it after adding
some.

How the game is shaded
----------------------
- Each DS screen is shaded on its own, at its native size (256x192, or wider with a
  widescreen aspect ratio), into its place in the window. With both screens shown,
  each screen runs its own copy of the shader.
- The picture is stretched to fill its place, not scaled by whole numbers. A shader
  that wants an exact size can say so in its preset (scale_type).
- System Settings and the game's menus are shaded too; the game's cutscenes and the
  messages in the top-left corner are not.

Requirements
------------
- OpenGL 3.3. Egg of Time draws with OpenGL when this folder has shaders, and with
  your system's default (Direct3D on Windows) otherwise.
- Slang shaders run in librashader, which comes with the Windows and Linux releases
  (librashader.dll / librashader.so beside the executable). On other systems, install
  librashader (https://github.com/SnowflakePowered/librashader): without it, only the
  GLSL shaders are listed.

If a shader does not load (a file missing, or it does not compile on your graphics
driver), the game says so and is drawn without it; the log (eggoftime.log, or the
terminal) gives the reason.
