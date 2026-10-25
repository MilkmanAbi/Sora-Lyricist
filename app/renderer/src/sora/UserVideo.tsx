// The user's own clip (video, GIF or picture) in place of the record. Sora's pipeline has already turned it into
// a silent H.264 loop at the video's exact size, so it just loops full-frame under a soft wash of the theme.
import { AbsoluteFill, Loop, OffthreadVideo, staticFile } from "remotion";
import { Look } from "../looks";
import { alpha, Background, SoraData, Theme } from "./theme";

export const UserVideo: React.FC<{ bg: Background; data: SoraData; look: Look; th: Theme }> = ({ bg, data, look, th }) => {
  const short = Math.min(data.width, data.height);
  return (
    <AbsoluteFill style={{ backgroundColor: th.bg }}>
      <Loop durationInFrames={Math.max(1, bg.frames)}>
        <OffthreadVideo src={staticFile(bg.src)} muted style={{ width: "100%", height: "100%", objectFit: "cover" }} />
      </Loop>
      {/* the theme's tint, deepest in the middle band where the lyrics sit */}
      <AbsoluteFill
        style={{
          background: [
            `radial-gradient(ellipse 75% 45% at 50% 50%, ${alpha(th.bg, 0.45)} 0%, transparent 100%)`,
            `linear-gradient(180deg, ${alpha(th.bg, 0.4)} 0%, ${alpha(th.bg, 0.5)} 50%, ${alpha(th.bg, 0.7)} 100%)`,
          ].join(", "),
        }}
      />
      {/* and a little of its colour over the whole clip, so every theme reads as itself ('Original' keeps the clip's) */}
      {th.id !== "original" ? <AbsoluteFill style={{ backgroundColor: th.bg2, mixBlendMode: "color", opacity: 0.35 }} /> : null}
      <AbsoluteFill style={{ background: `radial-gradient(ellipse at 50% 50%, ${alpha(th.accent, 0.1)} 0%, transparent 70%)` }} />
      {/* title tag, bottom left */}
      <div
        style={{
          position: "absolute",
          left: short * 0.06,
          bottom: short * 0.06,
          display: "flex",
          alignItems: "center",
          gap: short * 0.022,
          fontFamily: look.fontMain,
          color: th.ink,
          textShadow: `0 1px ${short * 0.02}px ${alpha(th.bg, 0.8)}`,
        }}
      >
        <div style={{ width: short * 0.022, height: short * 0.022, borderRadius: "50%", background: th.accent }} />
        <div style={{ fontWeight: 800, fontSize: short * 0.034, letterSpacing: "0.04em", textTransform: "uppercase" }}>
          {data.title || data.id}
        </div>
        {data.artist ? <div style={{ fontWeight: 500, fontSize: short * 0.03, opacity: 0.75 }}>{data.artist}</div> : null}
      </div>
    </AbsoluteFill>
  );
};
