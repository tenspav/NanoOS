from pathlib import Path


def check(report, volume):
    lines = Path(report).read_text().splitlines()
    assert lines and lines[0].startswith("NANOOS GL2 GAME build "), lines
    assert "HELLO PASS" in lines, lines
    row = next(l for l in lines if l.startswith("HELLO1 "))
    vals = [int(x, 16) for x in row.split()[2:8]]
    assert vals[0] == 0 and vals[1] == 30 and vals[2] == 0 and vals[3] == 0 and vals[4] == 0, vals
    assert (vals[5] & 0xFFFFFF) != 0, hex(vals[5])
    return {"pass": True, "frames": vals[1], "software": vals[2], "fault": vals[3], "center_px": hex(vals[5])}
