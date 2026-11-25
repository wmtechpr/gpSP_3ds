#include <cstdio>
#include <cstring>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>

#include "3dsutils.h"
#include "3dssettings.h"
#include "3dsglyphs.h"
#include "3dslog.h"
#include "3dsimpl_gpu.h"
#include "3dsui.h"
#include "3dsui_img.h"
#include "3dsui_notif.h"

enum NotifState {
    NOTIF_STATE_HIDDEN,
    NOTIF_STATE_ENTERING,
    NOTIF_STATE_VISIBLE,
    NOTIF_STATE_LEAVING,
};

static const u64 NOTIF_ANIMATION_TICKS = 150ULL * CPU_TICKS_PER_MSEC;
static const float NOTIF_ANIMATION_Y_OFFSET = 8.0f;

typedef struct {
    u64 stateStartedAt;
    u64 visibleDuration;
    u32 backgroundColor, borderColor, textColor;
    u16 bx0, by0, bx1, by1;
    u16 textWidth;
    u16 minTextWidth;
    u8 borderSize, paddingX, paddingY;

    char text[NOTIF_TEXT_MAX];
    Notif::Event event;
    Notif::Type type;
    NotifState state;
    bool dirty;
    bool persistent;
} PlainNotification;

static PlainNotification notifMsg = {0};
static PlainNotification notifFps = {0};
static PlainNotification notifMsgPending = {0};
static bool notifMsgHasPending = false;

static const int NOTIF_MARGIN = 8;

// Give each notification texture its own upload space so uploads cannot overwrite each other.
static const SGPU_TEXTURE_ID notifTextureIds[] = {
    UI_NOTIF_MSG, UI_NOTIF_FPS
};
static const int NOTIF_TEXTURE_COUNT = sizeof(notifTextureIds) / sizeof(notifTextureIds[0]);

static u8 *notifUploadBuffer = NULL;
static u8 *notifUploadSlice[NOTIF_TEXTURE_COUNT] = {0};

static u8 *notif3dsGetUploadSlice(SGPU_TEXTURE_ID id) {
    for (int i = 0; i < NOTIF_TEXTURE_COUNT; i++)
        if (notifTextureIds[i] == id)
            return notifUploadSlice[i];
    return NULL;
}

// Call after all notification textures are initialized.
static bool notif3dsInitUploadBuffer() {
    size_t total = 0;
    for (int i = 0; i < NOTIF_TEXTURE_COUNT; i++)
        total += GPU3DS.textures[notifTextureIds[i]].tex.size;

    notifUploadBuffer = (u8 *)linearAlloc(total);
    if (!notifUploadBuffer)
        return false;

    memset(notifUploadBuffer, 0, total);

    size_t offset = 0;
    for (int i = 0; i < NOTIF_TEXTURE_COUNT; i++) {
        notifUploadSlice[i] = notifUploadBuffer + offset;
        offset += GPU3DS.textures[notifTextureIds[i]].tex.size;
    }

    log3dsWrite("allocate notif upload buffer (%.2fkb)", (float)total / 1024);
    return true;
}

static bool notif3dsInitTexture(SGPU_TEXTURE_ID id, int maxWidth, int maxHeight, GPU_TEXCOLOR fmt = GPU_RGBA4, bool bilinear = false) {
    SGPUTexture *texture = &GPU3DS.textures[id];

    int width = gpu3dsGetNextPowerOf2(maxWidth);
    int height = gpu3dsGetNextPowerOf2(maxHeight);

    if (!C3D_TexInitVRAM(&texture->tex, width, height, fmt)) {
        return false;
    }

    texture->id = id;

    GPU_TEXTURE_FILTER_PARAM filter = bilinear ? GPU_LINEAR : GPU_NEAREST;
    C3D_TexSetFilter(&texture->tex, filter, filter);

    texture->scale[3] = 1.0f / texture->tex.width;
    texture->scale[2] = 1.0f / texture->tex.height;
    texture->scale[1] = 0;
    texture->scale[0] = 0;

    C3D_Tex *tex = &texture->tex;
    log3dsWrite("ui texture \"%s\" dim: %dx%d, size:%.2fkb, format: %s",
        gpu3dsTextureIDToString(id),
        tex->width, tex->height,
        (float)tex->size / 1024,
        gpu3dsTexColorToString(tex->fmt)
    );

    return true;
}

// Text uploads need a vertical flip.
static void notif3dsUpload(SGPU_TEXTURE_ID id, bool flipVert) {
    C3D_Tex *tex = &GPU3DS.textures[id].tex;
    u8 *slice = notif3dsGetUploadSlice(id);

    GSPGPU_FlushDataCache(slice, tex->size);

    C3D_SyncDisplayTransfer(
        (u32 *)slice,     GX_BUFFER_DIM(tex->width, tex->height),
        (u32 *)tex->data, GX_BUFFER_DIM(tex->width, tex->height),
        GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_FLIP_VERT(flipVert ? 1 : 0) |
        GX_TRANSFER_IN_FORMAT(tex->fmt) | GX_TRANSFER_OUT_FORMAT(tex->fmt)
    );
}

// Reset and stamp the opaque texel used for solid UI fills.
static u16 *notif3dsBeginTextTexture(SGPU_TEXTURE_ID id) {
    C3D_Tex *tex = &GPU3DS.textures[id].tex;
    u16 *dst = (u16 *)notif3dsGetUploadSlice(id);

    memset(dst, 0, tex->size);
    // single texel is enough under GPU_NEAREST
    dst[(tex->height - 1) * tex->width + (tex->width - 1)] = 0xFFFF;
    return dst;
}

static u16 notif3dsSyncTexture(SGPU_TEXTURE_ID id, const char *text, u32 color) {
    C3D_Tex *tex = &GPU3DS.textures[id].tex;
    u16 *dst = notif3dsBeginTextTexture(id);

    char trimmed[64];
    ui3dsEllipsize(text, trimmed, sizeof(trimmed), tex->width);

    u16 textWidth = ui3dsDrawStringToTexture(
        dst, trimmed,
        0, 0, tex->width, tex->height,
        color
    );

    notif3dsUpload(id, true);

    return textWidth;
}

static u32 notif3dsApplyOpacity(u32 color, float opacity) {
    u32 alpha = (u32)((color & 0xFF) * opacity + 0.5f);
    return (color & 0xFFFFFF00) | alpha;
}

static u64 notif3dsAnimationElapsed(u64 stateStartedAt, u64 now) {
    u64 elapsed = now > stateStartedAt ? now - stateStartedAt : 0;
    return elapsed < NOTIF_ANIMATION_TICKS ? elapsed : NOTIF_ANIMATION_TICKS;
}

static void notif3dsGetAnimation(NotifState state, u64 stateStartedAt, u64 now,
                                  float *yOffset, float *opacity) {
    *yOffset = 0.0f;
    *opacity = 1.0f;

    if (state != NOTIF_STATE_ENTERING && state != NOTIF_STATE_LEAVING)
        return;

    float progress = ui3dsSmoothstep((float)notif3dsAnimationElapsed(stateStartedAt, now) /
                                     (float)NOTIF_ANIMATION_TICKS);

    if (state == NOTIF_STATE_ENTERING) {
        *yOffset = NOTIF_ANIMATION_Y_OFFSET * (1.0f - progress);
        *opacity = progress;
    } else {
        *yOffset = NOTIF_ANIMATION_Y_OFFSET * progress;
        *opacity = 1.0f - progress;
    }
}

static void notif3dsStartLeaving(NotifState *state, u64 *stateStartedAt, u64 now) {
    if (*state == NOTIF_STATE_ENTERING) {
        *stateStartedAt = now - (NOTIF_ANIMATION_TICKS - notif3dsAnimationElapsed(*stateStartedAt, now));
        *state = NOTIF_STATE_LEAVING;
    } else if (*state == NOTIF_STATE_VISIBLE) {
        *stateStartedAt = now;
        *state = NOTIF_STATE_LEAVING;
    }
}

static bool notif3dsAdvance(NotifState *state, u64 *stateStartedAt, u64 visibleDuration, bool persistent, u64 now) {
    if (*state == NOTIF_STATE_ENTERING && now - *stateStartedAt >= NOTIF_ANIMATION_TICKS) {
        *state = NOTIF_STATE_VISIBLE;
        *stateStartedAt += NOTIF_ANIMATION_TICKS;
    }
    if (*state == NOTIF_STATE_VISIBLE && !persistent && now - *stateStartedAt >= visibleDuration) {
        *state = NOTIF_STATE_LEAVING;
        *stateStartedAt += visibleDuration;
    }
    return *state == NOTIF_STATE_LEAVING && now - *stateStartedAt >= NOTIF_ANIMATION_TICKS;
}

static void notif3dsGetNotificationText(Notif::Event event, char* out, size_t bufferSize) {
    switch (event) {
        case Notif::SaveState:
            snprintf(out, bufferSize, "Saved to Slot #%d", settings3DS.CurrentSaveSlot);
            break;
        case Notif::LoadState:
            snprintf(out, bufferSize, "Loaded Slot #%d", settings3DS.CurrentSaveSlot);
            break;
        case Notif::SavingState:
            snprintf(out, bufferSize, "Saving Slot #%d... ", settings3DS.CurrentSaveSlot);
            break;
        case Notif::SavingScreenshot:
            snprintf(out, bufferSize, "Saving screenshot...");
            break;
        case Notif::SlotChanged:
            snprintf(out, bufferSize, "Current Slot: #%d", settings3DS.CurrentSaveSlot);
            break;
        case Notif::Screenshot:
            snprintf(out, bufferSize, "Screenshot saved");
            break;
        case Notif::FastForward:
            snprintf(out, bufferSize, "Fast Forward %s", session3DS.turboMode ? "enabled" : "disabled");
            break;
        case Notif::Misc:
            snprintf(out, bufferSize, NOTIF_DEFAULT_ERROR);
            break;
        default:
            out[0] = '\0';
            break;
    }
}

static void notif3dsApplyStyle(PlainNotification &notif) {
    notif.textColor = 0xFFFFFFFF;
    u32 alpha = (u32)(0.85f * 255.0f);

    switch (notif.type) {
        case Notif::Type::Success:
            notif.backgroundColor = 0x13753A00 | alpha;
            break;
        case Notif::Type::Error:
            notif.backgroundColor = 0xDB3B2100 | alpha;
            break;
        case Notif::Type::Warning:
            notif.backgroundColor = 0xFF990000 | alpha;
            break;
        case Notif::Type::Info:
            notif.backgroundColor = 0x1F79D100 | alpha;
            break;
        default:
            notif.backgroundColor = 0xAA;
            break;
    }

    notif.borderColor = 0xFFFFFFFF;
    notif.borderSize  = notif.event != Notif::FPS ? 1 : 0;
    notif.paddingX    = notif.borderSize + 4;
    notif.paddingY    = notif.borderSize + 1;

    u16 maxHeight = NOTIF_TEXT_HEIGHT_MAX + notif.paddingY * 2;

    notif.by0 = notif.event != Notif::FPS ? (SCREEN_HEIGHT - NOTIF_MARGIN - maxHeight) : NOTIF_MARGIN;
    notif.by1 = notif.by0 + maxHeight;
}


bool notif3dsInitialize() {
    if (!notif3dsInitTexture(UI_NOTIF_MSG, NOTIF_MSG_WIDTH_MAX, NOTIF_TEXT_HEIGHT_MAX))
        return false;

    if (!notif3dsInitTexture(UI_NOTIF_FPS, NOTIF_FPS_WIDTH_MAX, NOTIF_TEXT_HEIGHT_MAX))
        return false;

    return notif3dsInitUploadBuffer();
}

void notif3dsFinalize() {
    if (notifUploadBuffer) {
        log3dsWrite("dealloc notif upload buffer");
        linearFree(notifUploadBuffer);
        notifUploadBuffer = NULL;
    }
}

static void notif3dsPreparePlain(PlainNotification *notif, Notif::Event event, Notif::Type type,
                                  double durationInMs, const char *miscMessage) {
    notif->event = event;
    notif->type = type;

    if (event == Notif::Misc) {
        snprintf(notif->text, sizeof(notif->text), "%s", miscMessage != NULL ? miscMessage : NOTIF_DEFAULT_ERROR);
    } else {
        notif3dsGetNotificationText(event, notif->text, sizeof(notif->text));
    }

    notif3dsApplyStyle(*notif);
    notif->visibleDuration = (u64)(durationInMs * CPU_TICKS_PER_MSEC);
    notif->dirty = true;
    notif->persistent = false;
    notif->minTextWidth = 0;
}

static void notif3dsStartPlain(PlainNotification *notif, u64 now) {
    notif->state = NOTIF_STATE_ENTERING;
    notif->stateStartedAt = now;
}

void notif3dsTrigger(Notif::Event event, Notif::Type type, double durationInMs, const char *miscMessage) {
    u64 now = svcGetSystemTick();

    if (notifMsg.state == NOTIF_STATE_HIDDEN) {
        notif3dsPreparePlain(&notifMsg, event, type, durationInMs, miscMessage);
        notif3dsStartPlain(&notifMsg, now);
        return;
    }

    notif3dsPreparePlain(&notifMsgPending, event, type, durationInMs, miscMessage);
    notifMsgHasPending = true;
    if (!notifMsg.persistent)
        notif3dsStartLeaving(&notifMsg.state, &notifMsg.stateStartedAt, now);
}

void notif3dsTriggerPersistent(Notif::Event event, Notif::Type type, const char *miscMessage) {
    u64 now = svcGetSystemTick();

    if (notifMsg.state == NOTIF_STATE_HIDDEN) {
        notif3dsPreparePlain(&notifMsg, event, type, 0, miscMessage);
        notifMsg.persistent = true;
        notif3dsStartPlain(&notifMsg, now);
        return;
    }

    notif3dsPreparePlain(&notifMsgPending, event, type, 0, miscMessage);
    notifMsgPending.persistent = true;
    notifMsgHasPending = true;
    notif3dsStartLeaving(&notifMsg.state, &notifMsg.stateStartedAt, now);
}

void notif3dsResolvePersistent(Notif::Event event, Notif::Type type, double durationInMs, const char *miscMessage) {
    if (!notifMsg.persistent ||
        (notifMsg.state != NOTIF_STATE_ENTERING && notifMsg.state != NOTIF_STATE_VISIBLE)) {
        notif3dsTrigger(event, type, durationInMs, miscMessage);
        return;
    }

    u16 minTextWidth = notifMsg.dirty ? 0 : notifMsg.textWidth;
    notif3dsPreparePlain(&notifMsg, event, type, durationInMs, miscMessage);
    notifMsg.minTextWidth = minTextWidth;
    if (notifMsg.state == NOTIF_STATE_VISIBLE)
        notifMsg.stateStartedAt = svcGetSystemTick();
}

void notif3dsFpsUpdate(float fps) {
    char newText[64];
    snprintf(newText, sizeof(newText), "%.1f", fps);

    if (strcmp(newText, notifFps.text) != 0) {
        snprintf(notifFps.text, sizeof(notifFps.text), "%s", newText);

        notifFps.event = Notif::FPS;
        notifFps.type = Notif::Type::Default;
        notif3dsApplyStyle(notifFps);

        notifFps.dirty = true;
    }
}

void notif3dsTick() {
    u64 now = svcGetSystemTick();

    if (notif3dsAdvance(&notifMsg.state, &notifMsg.stateStartedAt, notifMsg.visibleDuration,
                        notifMsg.persistent, now)) {
        if (notifMsgHasPending) {
            notifMsg = notifMsgPending;
            notifMsgHasPending = false;
            notif3dsStartPlain(&notifMsg, now);
        } else {
            notifMsg.state = NOTIF_STATE_HIDDEN;
        }
    }
}

void notif3dsSync() {
    if (notifMsg.state != NOTIF_STATE_HIDDEN && notifMsg.dirty) {
        notifMsg.textWidth = notif3dsSyncTexture(UI_NOTIF_MSG, notifMsg.text, notifMsg.textColor);
        notifMsg.dirty = false;
    }

    if (settings3DS.ShowFPS && notifFps.dirty) {
        notifFps.textWidth = notif3dsSyncTexture(UI_NOTIF_FPS, notifFps.text, 0xFFFFFFFF);
        notifFps.dirty = false;
    }
}

static bool notif3dsIsVisible(SGPU_TEXTURE_ID textureId) {
    bool paused = session3DS.emulatorState == EMUSTATE_PAUSEMENU;

    if (textureId == UI_NOTIF_FPS) {
        return settings3DS.ShowFPS && !paused;
    }

    return notifMsg.state != NOTIF_STATE_HIDDEN;
}

void notif3dsHide() {
    notifMsg.state = NOTIF_STATE_HIDDEN;
    notifMsgHasPending = false;
}

void notif3dsDraw(SGPU_TEXTURE_ID textureId, float xOffset) {
    if (!notif3dsIsVisible(textureId)) return;

    SGPUTexture *texture = &GPU3DS.textures[textureId];
    PlainNotification &notif = textureId == UI_NOTIF_MSG ? notifMsg : notifFps;

    int boxTextW = notif.textWidth > notif.minTextWidth ? notif.textWidth : notif.minTextWidth;
    int boxW = boxTextW + notif.paddingX * 2;
    notif.bx0 = textureId == UI_NOTIF_FPS ? settings3DS.GameScreenWidth - NOTIF_MARGIN - boxW
                                          : NOTIF_MARGIN;
    notif.bx1 = notif.bx0 + boxW;

    float yOffset = 0.0f;
    float opacity = 1.0f;
    if (textureId == UI_NOTIF_MSG)
        notif3dsGetAnimation(notif.state, notif.stateStartedAt, svcGetSystemTick(), &yOffset, &opacity);

    float x0 = notif.bx0 + notif.paddingX + xOffset;
    float y0 = notif.by0 + notif.paddingY + yOffset;
    float x1 = x0 + notif.textWidth;
    float y1 = y0 + NOTIF_TEXT_HEIGHT_MAX;

    int wx = texture->tex.width - 1;
    int wy = texture->tex.height - 1;

    gpu3dsAddQuadRect(
        notif.bx0 + xOffset, notif.by0 + yOffset, notif.bx1 + xOffset, notif.by1 + yOffset, wx, wy, 0,
        notif3dsApplyOpacity(notif.backgroundColor, opacity), notif3dsApplyOpacity(notif.borderColor, opacity), notif.borderSize
    );

    gpu3dsAddSimpleQuadVertexes(
        x0, y0, x1, y1,
        0, 0, notif.textWidth, NOTIF_TEXT_HEIGHT_MAX,
        0, notif3dsApplyOpacity(0xFFFFFFFF, opacity)
    );

    SVertexList *list = &GPU3DS.vertices[VBO_SCREEN];

    GPU3DS.currentRenderState.textureBind = textureId;
    GPU3DS.currentRenderState.textureEnv = TEX_ENV_MODULATE_COLOR;
    GPU3DS.currentRenderState.alphaBlending = ALPHA_BLENDING_ENABLED;

    gpu3dsDraw(list, NULL, list->count);
}
