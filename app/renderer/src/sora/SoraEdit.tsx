// Sora's composition: the Music Tool edit, optionally recoloured by a theme and/or over the user's own clip.
// With neither, it is exactly Music Tool's LyricEdit (unless the lyrics are Japanese / Chinese, whose words must sit
// flush: then the same look comes through Sora's lyrics layer).
import { AbsoluteFill, Html5Audio, staticFile, useCurrentFrame, useVideoConfig } from "remotion";
import { LyricEdit } from "../LyricEdit";
import { LOOKS } from "../looks";
import { LyricEditProps } from "../types";
import { SoraLyrics } from "./SoraLyrics";
import { ThemedVinyl } from "./ThemedVinyl";
import { hasSpaceless, SoraData, themedLook, themeFromLook } from "./theme";
import { UserVideo } from "./UserVideo";

export const SoraEdit: React.FC<LyricEditProps> = (props) => {
  const frame = useCurrentFrame();
  const { fps } = useVideoConfig();
  const data = props.data as SoraData | undefined;
  if (!data) return null;
  if (!data.theme && !data.background && !hasSpaceless(data)) return <LyricEdit {...props} />;

  const base = LOOKS[data.look] ?? LOOKS.vinyl;
  const th = data.theme ?? themeFromLook(base, data.accent ?? base.accent);
  const look = themedLook(base, th);
  const t = frame / fps;

  return (
    <AbsoluteFill style={{ backgroundColor: th.bg }}>
      {data.background ? <UserVideo bg={data.background} data={data} look={look} th={th} /> : <ThemedVinyl data={data} look={look} t={t} th={th} />}
      <AbsoluteFill
        style={{ background: `radial-gradient(ellipse at center, transparent 45%, rgba(0,0,0,${look.vignette}) 100%)`, pointerEvents: "none" }}
      />
      <SoraLyrics data={data} look={look} th={th} t={t} fps={fps} overVideo={!!data.background} />
      <Html5Audio src={staticFile(data.audio)} />
    </AbsoluteFill>
  );
};
