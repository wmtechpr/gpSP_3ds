#ifndef _3DSUI_NOTIF_H_
#define _3DSUI_NOTIF_H_

#include <3ds.h>
#include "3dsgpu.h"

#define NOTIF_MSG_WIDTH_MAX 256
#define NOTIF_FPS_WIDTH_MAX 64
#define NOTIF_TEXT_MAX 64
#define NOTIF_TEXT_HEIGHT_MAX 16

#define NOTIF_DEFAULT_DURATION 1200
#define NOTIF_DEFAULT_ERROR "Error. Something went wrong."

namespace Notif {
    enum Event {
        None = -1,
        SaveState,
        LoadState,
        SavingState,
        SavingScreenshot,
        SlotChanged,
        Screenshot,
        FastForward,
        Misc, // e.g. error messages
        FPS, // persistent overlay (top-left), separate texture
        Count
    };

    enum Type {
        Success,
        Error,
        Warning,
        Info,
        Default,
    };
}

bool notif3dsInitialize();
void notif3dsFinalize();
void notif3dsTrigger(Notif::Event event, Notif::Type type, double durationInMs = NOTIF_DEFAULT_DURATION, const char *miscMessage = NULL);
// Enters and stays visible until notif3dsResolvePersistent; other toasts queue behind it.
void notif3dsTriggerPersistent(Notif::Event event, Notif::Type type, const char *miscMessage = NULL);
// Replaces the persistent toast's text in place; without one, behaves like notif3dsTrigger.
void notif3dsResolvePersistent(Notif::Event event, Notif::Type type, double durationInMs = NOTIF_DEFAULT_DURATION, const char *miscMessage = NULL);
void notif3dsFpsUpdate(float fps);
void notif3dsTick();
void notif3dsSync();
void notif3dsHide();
void notif3dsDraw(SGPU_TEXTURE_ID textureId, float xOffset = 0.0f);

#endif
