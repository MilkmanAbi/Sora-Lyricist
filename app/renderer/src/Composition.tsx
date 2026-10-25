// Sora's copy of Music Tool's Composition.tsx (installer\assemble_app.ps1 puts it over the original): the same
// "LyricEdit" composition, drawn by SoraEdit, which adds video themes and the user's own background clip.
import { CalculateMetadataFunction, Composition, staticFile } from "remotion";
import { SoraEdit } from "./sora/SoraEdit";
import { EditData, LyricEditProps } from "./types";

const calculateMetadata: CalculateMetadataFunction<LyricEditProps> = async ({ props, abortSignal }) => {
  const res = await fetch(staticFile(`edits/${props.editId}.json`), { signal: abortSignal });
  const data = (await res.json()) as EditData;
  return {
    durationInFrames: data.durationInFrames,
    fps: data.fps,
    width: data.width,
    height: data.height,
    props: { ...props, data },
  };
};

export const MyComposition = () => {
  return (
    <Composition
      id="LyricEdit"
      component={SoraEdit}
      durationInFrames={300}
      fps={30}
      width={1080}
      height={1920}
      defaultProps={{ editId: "medusa_short" } as LyricEditProps}
      calculateMetadata={calculateMetadata}
    />
  );
};
