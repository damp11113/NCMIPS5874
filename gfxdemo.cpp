/*
 * gfxdemo: libgfx (GFX_NC5874 back-end) on the set-top box.
 *
 *   U-Boot: source avstart.scr (HDMI on), then  go ${a}
 *
 * Left: shapes drawn directly with the LinuxGFX API (anti-aliased circles,
 * thick lines, arcs, rounded rects, triangles, alpha blending, text).
 * Right: the same kind of scene recorded with DrawReplay, exported as a DRP1
 * blob and replayed with DRRender (), as an MCU sending commands would.
 * Bottom: a bouncing ball and the frame rate. STANDBY or a serial key exits.
 */
#include "GFX.h"
#include "DrawReplay.h"
#include "board.h"

static const uint32_t kBg = LinuxGFX::colorRGB(12, 20, 48);

static void drawDirect(LinuxGFX &g) {
    g.setText(40, 20, "libgfx on a Nationalchip 5874", GFX_WHITE, GFX_TRANSPARENT, 4, 4);
    g.setText(40, 60, "LinuxGFX API, ARGB8888 -> OSD ARGB1555", GFX_YELLOW, GFX_TRANSPARENT, 2, 2);

    g.fillRoundRect(40, 110, 260, 120, 24, LinuxGFX::colorRGB(40, 120, 200));
    g.drawRoundRect(40, 110, 260, 120, 24, GFX_WHITE);
    g.setText(60, 160, "fillRoundRect", GFX_WHITE, GFX_TRANSPARENT, 2, 2);

    g.setAntiAlias(true);
    g.setStrokeWidth(4);
    g.drawCircle(420, 170, 60, GFX_CYAN);
    g.drawArc(420, 170, 40, 0.0f, 270.0f, GFX_MAGENTA);
    g.drawLine(320, 260, 560, 330, GFX_GREEN);
    g.setStrokeWidth(1);
    g.setAntiAlias(false);

    g.fillTriangle(60, 330, 160, 250, 260, 330, GFX_RED);
    // Semi-transparent overlap: alpha 0x80 blends over the triangle
    g.fillCircle(200, 300, 45, LinuxGFX::colorARGB(0x80, 255, 255, 0));
}

static size_t drawReplayed(LinuxGFX &g) {
    DrawReplay rec(1280, 720);

    rec.fillRoundRect(660, 110, 560, 230, 16, LinuxGFX::colorRGB(30, 30, 30));
    rec.setText(680, 125, "DrawReplay -> DRP1 blob -> DRRender", GFX_WHITE, GFX_TRANSPARENT, 2, 2);
    for (int i = 0; i < 8; i++) {
        rec.fillCircle(720 + i * 62, 220, 24,
                        LinuxGFX::colorRGB((uint8_t) (i * 32), (uint8_t) (255 - i * 32), 160));
    }
    rec.drawRect(680, 270, 520, 50, GFX_YELLOW);
    rec.setText(695, 285, "vector commands, not pixels", GFX_YELLOW, GFX_TRANSPARENT, 2, 2);

    std::vector<uint8_t> blob = rec.DRExport();
    DRError err = DrawReplay::DRRender(blob.data(), blob.size(), g);
    if (err != DRError::Ok) {
        g.setText(680, 300, DRErrorString(err), GFX_RED, GFX_BLACK, 2, 2);
    }
    return blob.size();
}

int main(int argc, char *argv[]) {
    LinuxGFX gfx(1280, 720);
    char line[96];

    if (gfx.width() == 0) {
        puts("gfxdemo: display not running, source avstart.scr first\n");
        return 1;
    }

    gfx.fillScreen(kBg);
    drawDirect(gfx);
    size_t blobBytes = drawReplayed(gfx);
    snprintf(line, sizeof(line), "blob %u bytes vs %u bytes raw frame",
              (unsigned) blobBytes, (unsigned) (1280u * 720u * 4u));
    gfx.setText(680, 350, line, GFX_WHITE, GFX_TRANSPARENT, 2, 2);
    gfx.setText(40, 680, "STANDBY (or a serial key) exits", GFX_WHITE, GFX_TRANSPARENT, 2, 2);
    gfx.swapBuffers();

    int x = 100, y = 480, dx = 9, dy = 6, frames = 0;
    uint32_t t0 = get_timer(0), fps = 0;
    const int r = 30, top = 400, bottom = 660, left = 40, right = 1240;

    gfx.fillRect(left, top, right - left, bottom - top, kBg);
    while (!standby_pressed() && !tstc()) {
        // Erase only what moved: the old ball and the fps label. With the
        // dirty-rectangle present, swapBuffers () then converts just these.
        gfx.fillRect((int16_t) (x - r - 1), (int16_t) (y - r - 1), 2 * r + 3, 2 * r + 3, kBg);
        gfx.fillRect(left + 10, top + 10, 120, 16, kBg);
        x += dx;
        y += dy;
        if (x - r < left || x + r > right) {
            dx = -dx;
            x += 2 * dx;
        }
        if (y - r < top || y + r > bottom) {
            dy = -dy;
            y += 2 * dy;
        }
        gfx.setAntiAlias(true);
        gfx.fillCircle((int16_t) x, (int16_t) y, r, GFX_WHITE);
        gfx.setAntiAlias(false);

        snprintf(line, sizeof(line), "%u fps", (unsigned) fps);
        gfx.setText(left + 10, top + 10, line, GFX_GREEN, GFX_TRANSPARENT, 2, 2);
        gfx.swapBuffers();

        frames++;
        if (get_timer(t0) >= 1000) {
            fps = (uint32_t) frames;
            frames = 0;
            t0 = get_timer(0);
        }
    }
    if (tstc()) {
        getc();
    }
    while (standby_pressed()) {
        udelay(20000);
    }

    gfx.fillScreen(GFX_TRANSPARENT);
    gfx.clearBuffer(-1, 0);
    gfx.swapBuffers();
    puts("gfxdemo done\n");
    return 0;
}
