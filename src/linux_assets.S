/* Embedded assets for the native Linux GUI (GuiLinux.cpp). Assemble from the
   repository root so the .incbin paths resolve. */
    .section .rodata
    .balign 16
    .global asset_banner_bmp
    .global asset_banner_bmp_end
asset_banner_bmp:
    .incbin "banner.bmp"
asset_banner_bmp_end:

    .balign 16
    .global asset_icon_png
    .global asset_icon_png_end
asset_icon_png:
    .incbin "linux/mctde.png"
asset_icon_png_end:

    .balign 16
    .global asset_font_ui
    .global asset_font_ui_end
asset_font_ui:
    .incbin "third_party/imgui/fonts/Roboto-Medium.ttf"
asset_font_ui_end:

    .balign 16
    .global asset_font_mono
    .global asset_font_mono_end
asset_font_mono:
    .incbin "third_party/imgui/fonts/Cousine-Regular.ttf"
asset_font_mono_end:

    .section .note.GNU-stack,"",@progbits
