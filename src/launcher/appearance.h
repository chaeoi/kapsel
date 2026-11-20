#pragma once

void init_appearance(HWND dialog);
void release_appearance(void);
BOOL paint_dialog_background(HWND dialog, HDC dc);
BOOL paint_dialog_button(HWND dialog, const DRAWITEMSTRUCT *item);
INT_PTR color_dialog_control(UINT message, HDC dc, HWND control);
