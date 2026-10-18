# nice!view (custom)

Left-half (central) status screen for the Lulu58, based on the stock nice!view shield
and styled after [nice-view-gem](https://github.com/M165437/nice-view-gem).

From top to bottom:

- **SIG**: connection icon (Bluetooth connected, not connected, open for pairing, or USB)
- **BAT L / BAT R**: battery level of each half (`--` until the right half reports)
- **Profiles**: five squares for Bluetooth profiles 1-5. The active one is filled. A profile
  cleared with `&bt BT_CLR` or `&bt BT_CLR_ALL` shows an X until a device pairs to it (this
  resets on reboot).
- **Crystal**: nice-view-gem's crystal. It only animates while the HYPER key
  (`LS(LC(LA(LGUI)))`) is held.
- **Layer**: active layer name

This shield only supports the central half; the peripheral half has no display.

## Options

| Option                                     | Default | Description                                    |
| ------------------------------------------ | ------- | ---------------------------------------------- |
| `CONFIG_NICE_VIEW_CUSTOM_WIDGET_INVERTED`  | y       | White on black                                 |
| `CONFIG_NICE_VIEW_CUSTOM_ROTATE_180`       | y       | Rotate the screen 180 degrees (Lulu58 mount)   |
| `CONFIG_NICE_VIEW_CUSTOM_ANIMATION_MS`     | 960     | Length of one 16-frame crystal loop            |

## Credits

The Pixel Operator Mono font, icons and crystal animation in `assets/` and `widgets/screen.c`
come from nice-view-gem by Michael Schmidt-Voigt (MIT, see `assets/LICENSE-nice-view-gem`).
The crystal art is by Trixelized; Pixel Operator is by Jayvee Enaguas (CC0).
