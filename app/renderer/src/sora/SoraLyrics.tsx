// layers/Lyrics.tsx with theme colours and a soft highlight: a rounded wash of the theme's glow fades in behind
// the word being sung and lingers briefly after it, so the highlight seems to glide along the line.
import { measureText } from "@remotion/layout-utils";
import { AbsoluteFill, interpolate } from "remotion";
import { Look } from "../looks";
import { vinylLayout } from "../layers/Vinyl";
import { Track, Word } from "../types";
import { alpha, SoraData, spacelessEnd, spacelessStart, Theme } from "./theme";

const EXIT_FRAMES = 5;
const HL_IN = 0.08; // s: highlight fade-in at the word's start
const HL_OUT = 0.22; // s: and its fade-out after the word

type Region = { left: number; width: number; top: number };

const applyCase = (s: string, mode: Look["caseMode"]) =>
  mode === "upper" ? s.toUpperCase() : mode === "lower" ? s.toLowerCase() : s;

// long held words switch to the accent face, as in Music Tool; not in Japanese / Chinese, where the fallback CJK font
// would just flip between weights mid-line
// the look's fonts have Latin letters only: Japanese, Chinese, Korean and Indic lyrics come from Windows' UI fonts, one
// family per script (left to itself, Chrome takes kanji and kana from different fonts, in different weights)
const withFallback = (family: string) =>
  `${family}, "Yu Gothic UI", "Yu Gothic", "Meiryo", "Microsoft YaHei UI", "Malgun Gothic", "Nirmala UI", "Leelawadee UI", sans-serif`;

const isEmphasis = (w: Word) => w.end - w.start > 0.6 && w.text.length > 2 && !spacelessEnd(w.text);

const WordView: React.FC<{
  word: Word;
  t: number;
  fps: number;
  data: SoraData;
  look: Look;
  th: Theme;
  accent: string;
  fontSize: number;
  overVideo: boolean;
  glue: boolean; // flush against the next word (Japanese / Chinese)
}> = ({ word, t, fps, data, look, th, accent, fontSize, overVideo, glue }) => {
  const lead = data.timing.lead_ms / 1000;
  const fade = Math.max(data.timing.fade_frames, 1) / fps;
  const shownAt = word.start - lead;
  const emphasis = isEmphasis(word);

  const k = interpolate(t, [shownAt, shownAt + fade], [0, 1], { extrapolateLeft: "clamp", extrapolateRight: "clamp" });
  const singing = t >= word.start && t < word.end;
  const sung = t >= word.start;
  // over a clip the theme's faint 'upcoming' colour gets lost in the picture: upcoming words are the ink, half-faded
  const color = singing ? accent : sung || overVideo ? th.ink : th.dim;
  const upcoming = overVideo && !sung ? 0.5 : 1;
  const hit = singing ? interpolate(t, [word.start, word.start + 0.12], [1.06, 1], { extrapolateRight: "clamp" }) : 1;
  const hl = interpolate(t, [word.start, word.start + HL_IN, word.end, word.end + HL_OUT], [0, 1, 1, 0], {
    extrapolateLeft: "clamp",
    extrapolateRight: "clamp",
  });

  return (
    <span
      style={{
        position: "relative",
        display: "inline-block",
        marginRight: glue ? 0 : fontSize * 0.28,
        opacity: k * upcoming,
        filter: `blur(${(1 - k) * 6}px)`,
        transform: `translateY(${(1 - k) * fontSize * 0.12}px) scale(${hit})`,
        color,
        fontFamily: withFallback(emphasis ? look.fontAccent : look.fontMain),
        fontWeight: emphasis ? 400 : look.weightMain,
        fontSize: emphasis ? fontSize * 1.1 : fontSize,
        letterSpacing: emphasis ? "-0.01em" : "-0.02em",
        textShadow:
          [singing ? `0 0 ${fontSize * 0.35}px ${th.glow}` : "", overVideo ? `0 2px ${fontSize * 0.22}px ${alpha(th.bg, 0.8)}` : ""]
            .filter(Boolean)
            .join(", ") || "none",
      }}
    >
      {hl > 0 ? (
        <span
          style={{
            position: "absolute",
            left: "-0.16em",
            right: "-0.16em",
            top: "0.12em",
            bottom: "0.06em",
            borderRadius: "0.28em",
            background: th.glow,
            boxShadow: `0 0 ${fontSize * 0.3}px ${th.glow}`,
            opacity: hl,
            zIndex: -1,
          }}
        />
      ) : null}
      {word.chunks.map((ch, i) => (
        <span key={i} style={{ opacity: t >= ch.at - lead ? 1 : 0 }}>
          {applyCase(ch.text, look.caseMode)}
        </span>
      ))}
    </span>
  );
};

const TrackView: React.FC<{
  track: Track;
  data: SoraData;
  look: Look;
  th: Theme;
  accent: string;
  t: number;
  fps: number;
  region: Region;
  fontSize: number;
  overVideo: boolean;
}> = ({ track, data, look, th, accent, t, fps, region, fontSize: baseSize, overVideo }) => {
  let fontSize = baseSize;
  const lineH = baseSize * 1.22;
  const blockH = lineH * data.timing.max_lines_per_screen;
  const screen = track.screens.find((s) => t >= s.start && t < s.end);
  if (!screen) return null;
  // Japanese / Chinese units of a line join without a gap
  const glued = (line: number[], k: number) =>
    k + 1 < line.length && spacelessEnd(track.words[line[k]].text) && spacelessStart(track.words[line[k + 1]].text);
  const widest = Math.max(
    ...screen.lines.map((line) =>
      line.reduce((sum, wi, k) => {
        const w = track.words[wi];
        const emphasis = isEmphasis(w);
        const size = emphasis ? fontSize * 1.1 : fontSize;
        return (
          sum +
          (glued(line, k) ? 0 : size * 0.28) +
          measureText({
            text: applyCase(w.text, look.caseMode),
            fontFamily: withFallback(emphasis ? look.fontAccent : look.fontMain),
            fontSize: size,
            fontWeight: String(emphasis ? 400 : look.weightMain),
            letterSpacing: `${(emphasis ? -0.01 : -0.02) * size}px`,
          }).width
        );
      }, 0),
    ),
  );
  fontSize *= Math.min(1, region.width / widest);
  const next = track.screens[track.screens.indexOf(screen) + 1];
  const cut = !!next && next.start - screen.end < 0.3;
  const exit = cut
    ? 1
    : interpolate(t, [screen.end - EXIT_FRAMES / fps, screen.end], [1, 0], {
        extrapolateLeft: "clamp",
        extrapolateRight: "clamp",
      });
  return (
    <div
      style={{
        position: "absolute",
        left: region.left,
        width: region.width,
        top: region.top - blockH / 2,
        height: blockH,
        textAlign: overVideo ? "center" : look.align,
        opacity: exit,
        filter: `blur(${(1 - exit) * 8}px)`,
      }}
    >
      {screen.lines.map((line, li) => (
        <div key={li} style={{ height: lineH, lineHeight: `${lineH}px`, whiteSpace: "nowrap" }}>
          {line.map((wi, k) => (
            <WordView
              key={wi}
              word={track.words[wi]}
              t={t}
              fps={fps}
              data={data}
              look={look}
              th={th}
              accent={accent}
              fontSize={fontSize}
              overVideo={overVideo}
              glue={glued(line, k)}
            />
          ))}
        </div>
      ))}
    </div>
  );
};

// over a user's clip the lyrics sit in the middle of the frame (mashups: one track above, one below)
export const videoLyricsLayout = (w: number, h: number, dual: boolean) => {
  const portrait = h > w;
  const regions: Region[] = (dual ? [0.3, 0.7] : [0.5]).map((y) => ({ left: w * 0.08, width: w * 0.84, top: h * y }));
  const fontSize = portrait ? w * (dual ? 0.078 : 0.1) : h * (w / h < 1.5 ? (dual ? 0.085 : 0.11) : dual ? 0.072 : 0.09);
  return { regions, fontSize };
};

export const SoraLyrics: React.FC<{ data: SoraData; look: Look; th: Theme; t: number; fps: number; overVideo: boolean }> = ({
  data,
  look,
  th,
  t,
  fps,
  overVideo,
}) => {
  const dual = data.tracks.length > 1;
  const L = overVideo
    ? videoLyricsLayout(data.width, data.height, dual)
    : vinylLayout(data.width, data.height, data.height > data.width, dual);
  // mashups: the second voice takes the theme's record colour so the two read apart
  const accents = [th.accent, th.record];
  return (
    <AbsoluteFill>
      {data.tracks.map((track, i) => (
        <TrackView
          key={track.name}
          track={track}
          data={data}
          look={look}
          th={th}
          accent={accents[i % accents.length]}
          t={t}
          fps={fps}
          region={L.regions[i]}
          fontSize={L.fontSize * look.fontScale}
          overVideo={overVideo}
        />
      ))}
    </AbsoluteFill>
  );
};
