"""Cross-process CLI smoke, including independent local writers and restart recovery."""
import json
import pathlib
import subprocess
import sys
import tempfile

exe = sys.argv[1]
with tempfile.TemporaryDirectory(prefix="lakestore-cli-") as tmp:
    root = pathlib.Path(tmp)
    uri = (root / "objects").as_uri()

    def run(*args, success=True):
        result = subprocess.run([exe, *args, "--store", uri], text=True, capture_output=True)
        assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
        return json.loads(result.stdout) if success else result

    run("create", "--table", "t", "--schema", "id:int64,label:string")
    csv = root / "input.csv"
    csv.write_text('id,label\n1,"hello, world"\n2,\\N\n')
    v = run("append", "--table", "t", "--input", str(csv))["version"]
    assert run("scan", "--table", "t")["rows"] == [[1, "hello, world"], [2, None]]
    run("clone", "--source", "t", "--version", str(v), "--as", "clone")
    processes = []
    for i in range(8):
        path = root / f"writer-{i}.csv"
        path.write_text(f"id,label\n{i + 10},writer\n")
        processes.append(subprocess.Popen([exe, "append", "--table", "t", "--input", str(path), "--store", uri], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True))
    for process in processes:
        stdout, stderr = process.communicate(timeout=30)
        assert process.returncode == 0, (stdout, stderr)
    assert len(run("scan", "--table", "t")["rows"]) == 10
    assert len(run("scan", "--table", "clone")["rows"]) == 2
    run("restore", "--table", "t", "--version", str(v))
    assert len(run("scan", "--table", "t")["rows"]) == 2
    run("compact", "--table", "t")
    run("expire-snapshots", "--table", "t", "--keep-last", "1")
    run("scan", "--table", "t", "--version", str(v), success=False)
    run("gc", "--grace", "0ms", success=False)
    run("gc", "--grace", "0ms", "--dry-run")
    run("gc", "--grace", "0ms", "--offline")
    run("gc", "--grace", "0ms", "--offline")
    run("verify", "--table", "t")
    run("verify", "--table", "clone")
    result = run("scan", "--table", "t", "--where", "id = 1", "--columns", "label")
    assert result["rows"] == [["hello, world"]]
    run("scan", "--table", "t", "--parallelism", "0", success=False)
    run("scan", "--table", "t", "--bogus", "1", success=False)
    run("create", "--table", "../outside", "--schema", "id:int64", success=False)
    streamed = subprocess.run([exe, "scan", "--table", "t", "--store", uri,
                               "--stream", "--buffer-mib", "1"],
                              text=True, capture_output=True, check=True)
    records = [json.loads(line) for line in streamed.stdout.splitlines()]
    assert records[-1]["event"] == "scan_complete"
    assert records[-1]["rows"] == 2
    assert records[-1]["peak_reserved_bytes"] <= 1024 * 1024
    assert sorted(records[:-1], key=lambda r: r[0]) == [[1, "hello, world"], [2, None]]
    # Force a write failure without relying on whether a tiny output fills a pipe.
    with open("/dev/full", "w") if pathlib.Path("/dev/full").exists() else open("/dev/null", "w") as sink:
        if pathlib.Path("/dev/full").exists():
            failed = subprocess.run([exe, "scan", "--table", "t", "--store", uri, "--stream"],
                                    stdout=sink, stderr=subprocess.PIPE, text=True)
            assert failed.returncode != 0
print("CLI smoke passed")
