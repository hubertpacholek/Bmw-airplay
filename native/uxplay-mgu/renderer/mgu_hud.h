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
#endif
