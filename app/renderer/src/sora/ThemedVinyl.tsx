// The album-sleeve scene of layers/Vinyl.tsx (same layout: title strip, sleeve, record, wave), coloured by a theme.
import { fitText } from "@remotion/layout-utils";
import { AbsoluteFill, staticFile } from "remotion";
import { Look } from "../looks";
import { Rect, vinylLayout, Wave } from "../layers/Vinyl";
import { alpha, SoraData, Theme, titleInk } from "./theme";

const fmt = (s: number) => `${Math.floor(s / 60)}:${String(Math.floor(s % 60)).padStart(2, "0")}`;

const splitTitle = (title: string) => {
  if (title.length <= 14) return [title];
  const mid = title.length / 2;
  const cuts = [...title.matchAll(/,? /g)].map((m) => m.index ?? 0);
  const cut = cuts.sort((x, y) => Math.abs(x - mid) - Math.abs(y - mid))[0];
  return cut ? [title.slice(0, cut).replace(/,$/, ""), title.slice(cut).replace(/^,?\s*/, "")] : [title];
};

const Obi: React.FC<{ r: Rect; portrait: boolean; compact: boolean; data: SoraData; look: Look; th: Theme }> = ({
  r,
  portrait,
  compact,
  data,
  look,
  th,
}) => {
  const lines = splitTitle((data.title || data.id).toUpperCase());
  const room = portrait ? r.w * 0.66 : r.h * (compact ? 0.78 : 0.6);
  const maxSize = compact ? r.w * (lines.length > 1 ? 0.3 : 0.44) : portrait ? (lines.length > 1 ? 50 : 72) : lines.length > 1 ? 70 : 92;
  const size = Math.min(
    maxSize,
    ...lines.map((l) => fitText({ text: l, withinWidth: room, fontFamily: look.fontMain, fontWeight: 800 }).fontSize),
  );
  const meta = `SIDE A · ${data.version === "short" ? "EDIT" : "FULL"} · ${fmt(data.durationInFrames / data.fps)}`;
  const ink = titleInk(th);
  const small: React.CSSProperties = { fontFamily: look.fontMain, fontWeight: 500, letterSpacing: "0.08em", color: th.surfaceInk };
  const ring = (d: number, b: number) => (
    <div style={{ width: d, height: d, borderRadius: "50%", border: `${b}px solid ${ink}`, boxSizing: "border-box" }} />
  );
  const titleBlock = (
    <div style={{ display: "flex", flexDirection: "column", alignItems: portrait ? "flex-start" : "center", gap: 6 }}>
      {lines.map((l) => (
        <div key={l} style={{ fontFamily: look.fontMain, fontWeight: 800, color: ink, fontSize: size, lineHeight: 0.95, whiteSpace: "nowrap" }}>
          {l}
        </div>
      ))}
      <div style={{ ...small, fontSize: compact ? r.w * 0.17 : portrait ? 26 : 28, fontWeight: 700, whiteSpace: "nowrap", marginTop: compact ? 2 : 6 }}>
        {data.artist ?? ""}
      </div>
    </div>
  );
  return (
    <div
      style={{
        position: "absolute",
        left: r.x,
        top: r.y,
        width: r.w,
        height: r.h,
        background: th.surface,
        boxShadow: "0 0 40px rgba(0,0,0,0.35)",
        overflow: "hidden",
        zIndex: 3,
        display: "flex",
        flexDirection: portrait ? "row" : "column",
        alignItems: "center",
        justifyContent: "space-between",
        padding: portrait ? `0 ${r.w * 0.06}px` : `${r.w * 0.16}px 0`,
        boxSizing: "border-box",
      }}
    >
      {portrait ? (
        <>
          {titleBlock}
          <div style={{ display: "flex", alignItems: "center", gap: 18 }}>
            <div style={{ ...small, fontSize: 20, writingMode: "vertical-rl" }}>{meta}</div>
            {ring(70, 6)}
          </div>
        </>
      ) : compact ? (
        <>
          <div style={{ height: r.w * 0.42 }} />
          <div style={{ writingMode: "vertical-rl" }}>{titleBlock}</div>
          {ring(r.w * 0.42, Math.max(2, r.w * 0.04))}
        </>
      ) : (
        <>
          <div style={{ ...small, fontSize: 20, writingMode: "vertical-rl" }}>{meta}</div>
          <div style={{ writingMode: "vertical-rl" }}>{titleBlock}</div>
          {ring(r.w * 0.5, 6)}
        </>
      )}
    </div>
  );
};

const Disc: React.FC<{ cx: number; cy: number; r: number; t: number; th: Theme; title: string; look: Look }> = ({
  cx,
  cy,
  r,
  t,
  th,
  title,
  look,
}) => {
  const angle = t * (100 / 3 / 60) * 360; // 33 1/3 rpm
  const g = Math.max(1.2, r * 0.008);
  return (
    <div style={{ position: "absolute", left: cx - r, top: cy - r, width: r * 2, height: r * 2, zIndex: 1 }}>
      <div
        style={{
          position: "absolute",
          inset: 0,
          borderRadius: "50%",
          transform: `rotate(${angle}deg)`,
          background: `repeating-radial-gradient(circle at 50% 50%, #0d0d10 0px, #0d0d10 ${g}px, #1c1b21 ${g * 1.4}px, #0d0d10 ${g * 2}px)`,
          boxShadow: `0 0 ${r * 0.25}px rgba(0,0,0,0.5)`,
        }}
      >
        <div
          style={{
            position: "absolute",
            left: r * 0.64,
            top: r * 0.64,
            width: r * 0.72,
            height: r * 0.72,
            borderRadius: "50%",
            overflow: "hidden",
            background: th.record,
          }}
        >
          <div style={{ position: "absolute", left: 0, right: 0, top: "58%", height: "14%", background: "rgba(22,19,28,0.45)" }} />
          {r > 120 ? (
            <div
              style={{
                position: "absolute",
                left: r * 0.11,
                right: r * 0.11,
                top: r * 0.12,
                fontFamily: look.fontMain,
                fontWeight: 800,
                fontSize: r * 0.06,
                color: "#16131c",
                textTransform: "uppercase",
                textAlign: "center",
                lineHeight: 1.05,
              }}
            >
              {title}
            </div>
          ) : null}
          <div
            style={{
              position: "absolute",
              left: r * 0.33,
              top: r * 0.33,
              width: r * 0.06,
              height: r * 0.06,
              borderRadius: "50%",
              background: "#0d0d10",
            }}
          />
        </div>
      </div>
      <div
        style={{
          position: "absolute",
          inset: 0,
          borderRadius: "50%",
          background:
            "conic-gradient(from 20deg, transparent 0deg, rgba(255,255,255,0.10) 30deg, transparent 70deg, transparent 180deg, rgba(255,255,255,0.07) 210deg, transparent 250deg)",
        }}
      />
    </div>
  );
};

export const ThemedVinyl: React.FC<{ data: SoraData; look: Look; t: number; th: Theme }> = ({ data, look, t, th }) => {
  const L = vinylLayout(data.width, data.height, data.height > data.width, data.tracks.length > 1);
  const s = L.sleeve;
  // the record sits on the theme's record colour so its outline reads; the sleeve is the theme's own gradient
  const backdrop = `radial-gradient(ellipse at 75% 50%, color-mix(in srgb, ${th.record} 85%, white) 0%, ${th.record} 45%, color-mix(in srgb, ${th.record} 65%, black) 100%)`;
  const tint = `radial-gradient(ellipse at 50% 45%, ${alpha(th.accent, 0.1)} 0%, transparent 65%)`;
  return (
    <AbsoluteFill style={{ background: backdrop, isolation: "isolate" }}>
      <Disc cx={L.disc.cx} cy={L.disc.cy} r={L.disc.r} t={t} th={th} title={data.title || data.id} look={look} />
      <div
        style={{
          position: "absolute",
          left: s.x,
          top: s.y,
          width: s.w,
          height: s.h,
          overflow: "hidden",
          zIndex: 2,
          background: `${tint}, linear-gradient(160deg, ${th.bg2} 0%, ${th.bg} 100%)`,
          boxShadow: "20px 0 50px rgba(0,0,0,0.45)",
        }}
      />
      <Obi r={L.obi} portrait={L.mode === "portrait"} compact={L.mode === "compact"} data={data} look={look} th={th} />
      <Wave r={L.wave} audio={staticFile(data.audio)} color={th.ink} stroke={L.mode === "compact" ? 2 : 2.5} opacity={0.85} />
    </AbsoluteFill>
  );
};
