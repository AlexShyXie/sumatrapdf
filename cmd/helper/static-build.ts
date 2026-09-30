// Static release build used by cmd/build.ts -static.
// Builds the single-file out/rel64/SumatraPDF-static.exe
import { writeFileSync } from "node:fs";
import { join } from "node:path";
import { getGitLinearVersion, getGitSha1, runLogged, detectVisualStudio2026 } from "../util";

const { msbuildPath } = detectVisualStudio2026();
const slnPath = join("vs2022", "SumatraPDF.sln");

function buildConfigPath(): string {
  return join("src", "BuildConfig.h");
}

function setBuildConfigPreRelease(sha1: string, preRelVer: string): void {
  const todayDate = new Date().toISOString().slice(0, 10);
  let s = `#define GIT_COMMIT_ID ${sha1}\n`;
  s += `#define BUILT_ON ${todayDate}\n`;
  s += `#define PRE_RELEASE_VER ${preRelVer}\n`;
  writeFileSync(buildConfigPath(), s, "utf-8");
}

async function revertBuildConfig(): Promise<void> {
  const proc = Bun.spawn(["git", "checkout", buildConfigPath()], {
    stdout: "inherit",
    stderr: "inherit",
  });
  await proc.exited;
}

export async function buildStatic() {
  const timeStart = performance.now();
  const preRelVer = String(await getGitLinearVersion());
  const sha1 = await getGitSha1();
  console.log(`building static release version ${preRelVer}`);

  // the prebuild packs .work/docs into IDR_EMBEDDED_PAK; on CI without the
  // website checkout genDocs skips itself and the exe ships without the manual
  const { main: genDocs } = await import("../gen-docs");
  await genDocs();

  setBuildConfigPreRelease(sha1, preRelVer);
  try {
    await runLogged(msbuildPath, [
      slnPath,
      "/t:SumatraPDF-static:Rebuild",
      "/p:Configuration=Release;Platform=x64",
      "/m",
    ]);
  } finally {
    await revertBuildConfig();
  }

  console.log(`exe: ${join("out", "rel64", "SumatraPDF-static.exe")}`);
  const elapsed = ((performance.now() - timeStart) / 1000).toFixed(1);
  console.log(`buildStatic finished in ${elapsed}s`);
}
