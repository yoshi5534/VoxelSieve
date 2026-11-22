"""The inspection of the sample part (docs/quickstart.md) through the MCP server as a client
drives it: the program itself over standard input and output, one request after the answer to
the one before, with a project, the import, the analyses, the report, views, undo and redo.
The scan is synthetic, so the answers are known: three shrinkage cavities and one zone of
loosened microstructure.

    python mcp_walk.py <vs-gui> <vs-synth> <inspection_order.json> <work dir>
"""

import json
import pathlib
import shutil
import subprocess
import sys


class Server:
    """vs-gui --mcp as a child process."""

    def __init__(self, program, root):
        self.process = subprocess.Popen(
            [program, "--mcp", "--no-reopen", str(root)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, encoding="utf-8")
        self.requests = 0

    def request(self, method, params=None):
        self.requests += 1
        message = {"jsonrpc": "2.0", "id": self.requests, "method": method}
        if params is not None:
            message["params"] = params
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise SystemExit(f"vs-gui ended during {method}")
            answer = json.loads(line)
            if answer.get("id") == self.requests:
                if "error" in answer:
                    raise SystemExit(f"{method}: {answer['error']}")
                return answer["result"]

    def notify(self, method):
        self.process.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method}) + "\n")
        self.process.stdin.flush()

    def call(self, tool, **arguments):
        """The content of a tool's answer: (JSON of its text, kinds of content)."""
        result = self.request("tools/call", {"name": tool, "arguments": arguments})
        texts = [c["text"] for c in result["content"] if c["type"] == "text"]
        kinds = [c["type"] for c in result["content"]]
        return json.loads(texts[-1]) if texts else {}, kinds, result.get("isError", False)

    def done(self, tool, **arguments):
        """A tool call that must succeed."""
        answer, kinds, failed = self.call(tool, **arguments)
        if failed:
            raise SystemExit(f"{tool} failed: {json.dumps(answer)[:600]}")
        return answer, kinds

    def close(self):
        self.process.stdin.close()
        return self.process.wait(timeout=60)


def expect(condition, what):
    if not condition:
        raise SystemExit(f"Not as expected: {what}")


def main():
    gui, synth, order, work = sys.argv[1:5]
    work = pathlib.Path(work).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    # The sample data of a release (packaging/make-samples.cmake).
    subprocess.run(
        [synth, "--part", "housing", "--voxel-size", "0.25", "--lunker", "3", "--loosening", "1",
         "--noise", "400", "--blur", "0.1", "--cupping", "0.05", "--stl", "housing.stl",
         "--out", "housing"],
        cwd=work, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # The server reads only below its root, so the order goes there too.
    shutil.copy(order, work / "inspection_order.json")

    server = Server(gui, work)
    try:
        walk(server, work)
    finally:
        server.process.kill()
    print("The walk through the sample part answered as expected")


def walk(server, work):
    hello = server.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                          "clientInfo": {"name": "walk", "version": "1"}})
    expect("serverInfo" in hello, "the server introduces itself")
    server.notify("notifications/initialized")
    tools = {tool["name"] for tool in server.request("tools/list")["tools"]}
    expect({"project_create", "run_import_raw", "run_report", "view_slice"} <= tools,
           "the tools of an inspection are offered")

    project, _ = server.done("project_create", path=str(work / "walk.vsproj"), name="Walk")
    expect(project["name"] == "Walk" and project["steps"] == [], "an empty project")

    imported, _ = server.done("run_import_raw", path=str(work / "housing.raw"))
    expect(imported["status"] == "done" and imported["object"] == "o1", "the import makes o1")

    # The ground truth of the scan.
    porosity, _ = server.done("run_porosity")
    expect(porosity["summary"]["pores"] == 3, f"three cavities: {porosity['summary']}")
    expect(porosity["summary"]["zones"] == 1, f"one loosened zone: {porosity['summary']}")

    # The comparison needs a surface, which runs as a step of its own before it.
    compared, _ = server.done("run_compare_cad", cad_path=str(work / "housing.stl"))
    before = compared.get("preceded_by", [])
    expect(len(before) == 1 and before[0]["operation"] == "surface", f"a surface step: {before}")
    expect(compared["inputs"]["surface"]["step"] == before[0]["id"], "the comparison uses it")

    thickness, _ = server.done("run_wall_thickness", min_wall_mm=1.0)
    expect(thickness["inputs"]["surface"]["step"] == before[0]["id"],
           "the wall thickness uses the same surface")

    report, _ = server.done("run_report", order_path=str(work / "inspection_order.json"))
    expect(report["summary"]["passed"] is True, f"the sample part passes: {report['summary']}")
    files, _ = server.done("list_files", step=report["id"])
    names = {entry["file"] for entry in files["files"]}
    expect({"report.html", "report.json"} <= names, f"the report files: {names}")
    expect((pathlib.Path(files["path"]) / "report.html").stat().st_size > 10000,
           "the report is on disk")

    view, kinds = server.done("view_slice", axis="z")
    expect("image" in kinds and view["porosity_step"] == porosity["id"], "a slice with its pores")
    view, kinds = server.done("view_objects")
    expect("image" in kinds and view["objects"][0]["shape"] == "surface", "a view of the surface")

    undone, _ = server.done("undo")
    expect(undone["can_redo"] and undone["cursor"] == report["id"] - 1, "undo takes the report back")
    redone, _ = server.done("redo")
    expect(not redone["can_redo"] and redone["cursor"] == report["id"], "redo brings it back")

    results, _ = server.done("results")
    kinds_of_results = {result["type"] for result in results["results"]}
    expect({"porosity", "surface", "comparison", "wall_thickness", "report"} <= kinds_of_results,
           f"every analysis left a result: {kinds_of_results}")

    # A mistake is an error that says what is meant, and leaves no step behind.
    answer, _, failed = server.call("run_report", order={"acceptence": {}})
    expect(failed and "did you mean 'acceptance'" in answer.get("error", ""),
           f"a misspelt section is refused: {answer}")
    status, _ = server.done("project_status")
    expect(status["cursor"] == report["id"], "the refused report is no step")

    expect(server.close() == 0, "vs-gui ends with the input")


if __name__ == "__main__":
    main()
