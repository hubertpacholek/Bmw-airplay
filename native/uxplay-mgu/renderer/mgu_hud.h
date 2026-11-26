/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * mgu_hud.h - the picture on the head-up display (renderer/mgu_hud.c).
 */
#ifndef MGU_HUD_H
#define MGU_HUD_H
/* starts the HUD's thread once; its events go to emit_json */
void mgu_hud_init(void (*emit_json)(const char *json));
/* "hud on": wanted for the next 3 s (asked again to stay on); "hud off": not wanted from now */
void mgu_hud_want(int on);
/* the layout: size (20..100 % of the HUD), place (0..100 % of the room left: 0 left or top, 50 the middle), packed
 * (cluster-render's packed 4x4 blocks where it offers them: a third of the bytes, smoother) */
void mgu_hud_layout(int size, int x, int y, int packed);
/* how far ahead of the screen a mirrored picture goes (0..500 ms): the cluster's own delay */
void mgu_hud_lead(int ms);
#endif
