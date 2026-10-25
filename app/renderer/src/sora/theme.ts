// Sora's additions to the Music Tool renderer: video themes and a user-chosen background clip.
// Both arrive in the edit JSON (written by Sora's pipeline/mt_pipeline.py); without them the original look renders.
import { Look } from "../looks";
import { EditData } from "../types";

export type Theme = {
  id: string;
  name: string;
  bg: string; // sleeve / video tint (6-digit hex)
  bg2: string; // second sleeve colour (gradient)
  surface: string; // title strip
  surfaceInk: string; // title strip text
  ink: string; // sung text
  dim: string; // upcoming text
  accent: string; // the word being sung
  glow: string; // its soft highlight
  record: string; // record label + the colour behind the record
};

export type Background = { src: string; frames: number }; // public/ path of a looping, silent clip

export type SoraData = EditData & { theme?: Theme; background?: Background };

// 'Original' with a video background: colours from the Music Tool look and the song's own colour
export const themeFromLook = (look: Look, accent: string): Theme => ({
  id: "original",
  name: "Original",
  bg: "#0c0b10",
  bg2: "#1a1722",
  surface: "#f3efe6",
  surfaceInk: "#16131c",
  ink: look.ink,
  dim: look.dim,
  accent,
  glow: `${accent}33`,
  record: accent,
});

// the look the shared layers (title strip fonts, sizes) read, recoloured by the theme
export const themedLook = (look: Look, th: Theme): Look => ({ ...look, ink: th.ink, dim: th.dim, accent: th.accent });

// WCAG contrast ratio of two "#rrggbb" colours (1..21)
const lum = (hex: string) => {
  const c = [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16) / 255).map((v) => (v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4));
  return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
};
const contrast = (a: string, b: string) => {
  const [x, y] = [lum(a), lum(b)].sort((p, q) => q - p);
  return (x + 0.05) / (y + 0.05);
};

// the title on the title strip: the theme's accent where it stands out on the strip, else its record colour,
// else the strip's own ink (e.g. Darker's white accent on a near-white strip)
export const titleInk = (th: Theme) => {
  const hex = /^#[0-9a-f]{6}$/i;
  if (!hex.test(th.surface)) return th.surfaceInk;
  for (const c of [th.accent, th.record]) if (hex.test(c) && contrast(c, th.surface) >= 2.6) return c;
  return th.surfaceInk;
};

// "#rrggbb" + alpha 0..1 -> "#rrggbbaa"
export const alpha = (hex: string, a: number) =>
  /^#[0-9a-f]{6}$/i.test(hex) ? hex + Math.round(Math.max(0, Math.min(1, a)) * 255).toString(16).padStart(2, "0") : hex;
