"""Pinned SDL integration: let Aura own one bounded native clipboard per connection."""
from pathlib import Path
from dependencies import ROOT
BEFORE='    } else if (SDL_strcmp(interface, "wl_data_device_manager") == 0) {'
AFTER='''    /* Aura integration: avoid two clipboard/data-device owners on one connection.
     * The application's native service also owns future drag-and-drop handling. */
    } else if (SDL_strcmp(interface, "wl_data_device_manager") == 0 &&
               !SDL_GetHintBoolean("AURA_WAYLAND_EXTERNAL_CLIPBOARD", false)) {'''
def patch():
    target=ROOT/'third_party/src/sdl3/src/video/wayland/SDL_waylandvideo.c'
    text=target.read_text()
    if AFTER not in text:
        if text.count(BEFORE)!=1:raise RuntimeError('Pinned SDL clipboard patch does not match source')
        target.write_text(text.replace(BEFORE,AFTER,1))
    events=target.with_name('SDL_waylandevents.c');text=events.read_text()
    before='\n    if (seat->data_device->selection_offer) {'
    after='\n    if (seat->data_device && seat->data_device->selection_offer) {'
    if after not in text:
        if text.count(before)!=1:raise RuntimeError('Pinned SDL keyboard-leave patch does not match source')
        events.write_text(text.replace(before,after,1))
if __name__=='__main__':patch()
