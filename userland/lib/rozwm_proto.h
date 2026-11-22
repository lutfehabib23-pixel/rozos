/* rozOS — rozwm (window server) আর অ্যাপের মধ্যে কথা বলার নিয়ম (ধাপ ১৪)
 *
 * পথ: Unix socket ROZWM_SOCKET। প্রতিটা বার্তা একই মাপের (struct wm_msg, 96 বাইট) — stream
 * থেকে টুকরো এলে জমিয়ে পুরো হলে পড়া হয়।
 *
 * অ্যাপ → rozwm                         rozwm → অ্যাপ
 *   WM_HELLO     a = pid                  WM_CREATED  win, a=w b=h c=stride
 *   WM_CREATE    a=w b=h c=x d=y (−1 =         text = shm-এর নাম (পিক্সেল এখানে, 0xAARRGGBB)
 *                নিজে বাছো), flags, text=শিরোনাম  WM_KEY      a = কী (ASCII বা GFX_KEY_*)
 *   WM_DAMAGE    win, a b c d = x y w h      WM_MOUSE    flags = GFX_EV_MOUSE_*, a,b = x,y
 *   WM_TITLE     win, text                       (উইন্ডোর ভেতরের স্থানাঙ্ক), c = buttons,
 *   WM_DESTROY   win                              d = button বা wheel
 *   WM_QUIT      a = 't'/'r'/'p' — ডেস্কটপ বন্ধ (textmode কমান্ড)
 *   WM_KEEP_OPEN win — ধাপ ১৪খ: [X]-এর পর অ্যাপ প্রশ্ন করছে ("Save?"),
 *                ৩ সেকেন্ডের kill বাতিল
 *                                         WM_CLOSE    [X] চাপা হয়েছে (৩ সেকেন্ডে না গেলে kill)
 *                                         WM_RESIZE   a=w b=h (বাফার একই; শুধু মাপ বদলায়)
 *                                         WM_FOCUS    a = 1 সামনে / 0 পেছনে
 *                                         WM_RAWKEY   ধাপ ১৮ক: a = কাঁচা কী-ঘটনা (/dev/keyev-এর
 *                                                     int32: চাপা/ছাড়া), সামনের উইন্ডোতে
 *
 * পিক্সেল-বাফার: rozwm shm_open দিয়ে বানায়, পর্দার মাপের (stride = পর্দার চওড়া), তাই
 * মাপ বদলালেও নতুন বাফার লাগে না। অ্যাপ আঁকে, তারপর WM_DAMAGE পাঠায়; rozwm নিজের
 * back buffer-এ সাজিয়ে পর্দায় কপি করে (কখনো আধা-আঁকা ছবি পর্দায় যায় না — ঝিকমিক নেই)। */
#ifndef ROZWM_PROTO_H
#define ROZWM_PROTO_H

#include <stdint.h>

#define ROZWM_SOCKET "/tmp/.rozwm"

enum {
    WM_HELLO = 1, WM_CREATE, WM_DAMAGE, WM_TITLE, WM_DESTROY,
    WM_QUIT,                       /* a = 't' টেক্সট মোড, 'r' restart, 'p' poweroff (textmode প্রোগ্রাম) */
    WM_KEEP_OPEN,                  /* ধাপ ১৪খ: WM_CLOSE-এর পর জোর করে বন্ধ করা বাতিল */
    WM_CREATED = 100, WM_KEY, WM_MOUSE, WM_CLOSE, WM_RESIZE, WM_FOCUS,
    WM_RAWKEY                      /* ধাপ ১৮ক */
};

/* WM_CREATE flags */
#define WMF_FIXED_SIZE 0x01        /* মাপ বদলানো যাবে না */
#define WMF_CENTER     0x02        /* পর্দার মাঝখানে */

struct wm_msg {
    uint32_t type;
    uint32_t win;
    int32_t a, b, c, d;
    uint32_t flags;
    uint32_t reserved;
    char text[64];
};

#endif
