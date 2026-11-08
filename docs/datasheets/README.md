# Datasheets

Vendor datasheets are not redistributed in this repository. Download them into this folder if you want local copies
(`*.pdf` here is gitignored):

| Part | Document |
|---|---|
| ESP32-S3 SoC | [esp32-s3_datasheet_en.pdf](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf), [technical reference manual](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf) |
| ESP32-S3-WROOM-1 module | [esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf) |
| TMUX1511 isolation switch (TI) | [tmux1511.pdf](https://www.ti.com/lit/ds/symlink/tmux1511.pdf) |

The 3D model of the USB-C receptacle (HRO TYPE-C-31-M-12, [LCSC C165948](https://www.lcsc.com/product-detail/C165948.html))
is not included either. KiCad shows the board without it; to restore it, save the vendor STEP file as
`hardware/rev5/kicad/3dmodels/USB_C_HRO_TYPE-C-31-M-12_C165948.step`.
