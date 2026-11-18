# PlatformIO pre-build script: inject Git short SHA + dirty flag as FW_GIT_SHA.
import subprocess

Import("env")


def _git_output(args, project_dir):
    return subprocess.run(
        ["git", *args],
        cwd=project_dir,
        capture_output=True,
        text=True,
        check=False,
    )


def get_git_sha(project_dir):
    res = _git_output(["rev-parse", "--short=7", "HEAD"], project_dir)
    if res.returncode != 0:
        return "unknown"
    sha = res.stdout.strip()
    if not sha:
        return "unknown"
    dirty_wt = _git_output(["diff", "--quiet"], project_dir).returncode != 0
    dirty_idx = _git_output(["diff", "--cached", "--quiet"], project_dir).returncode != 0
    if dirty_wt or dirty_idx:
        sha += "-dirty"
    return sha


git_sha = get_git_sha(env.subst("$PROJECT_DIR"))
env.Append(CPPDEFINES=[("FW_GIT_SHA", env.StringifyMacro(git_sha))])
