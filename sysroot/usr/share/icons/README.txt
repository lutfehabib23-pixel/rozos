rozOS icons (phase 14b)

Put a picture here and the desktop uses it instead of the drawn symbol.
Formats: .png (with transparency), .bmp, .jpg, .gif, .tga, .ppm - any size,
it is scaled (32x32 on the desktop, 16x16 in menus, lists and toolbars).
After adding a file, restart the program (or the desktop) to see it.

Desktop:      terminal  files  code  calculator  paint  images  help
Files:        folder  file  c-file  image-file  program
Toolbars:     back  up  home  new-folder  rename  delete  copy  paste
              new  open  save  compile  run  find
Start menu:   the name in the "icon:" line of a .app file
              (/usr/share/apps/*.app), e.g.  icon: code

Example:  cp /home/me/folder.png /usr/share/icons/folder.png
In your own program:  ui_draw_icon(s, x, y, 32, "myicon", 'p');   (help rozui)
