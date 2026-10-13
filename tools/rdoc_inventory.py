# run inside qrenderdoc: qrenderdoc --python tools/rdoc_inventory.py
# env RDOC_CAP=<capture>, RDOC_OUT=<dir>. dumps an action list and the textures read by the
# last draws (where the post-process / tonemap passes live) as png
import json
import os

import renderdoc as rd

cap_path = os.environ["RDOC_CAP"]
out = os.environ["RDOC_OUT"]
os.makedirs(out, exist_ok=True)

cap = rd.OpenCaptureFile()
cap.OpenFile(cap_path, "", None)
status, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
assert status == rd.ResultCode.Succeeded, str(status)

actions = []


def walk(a, depth=0):
    for c in a:
        actions.append(c)
        walk(c.children, depth + 1)


walk(ctl.GetRootActions())
draws = [a for a in actions if a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch)]

report = {"actions": len(actions), "draws": len(draws), "tail": []}
res_names = {r.resourceId: r.name for r in ctl.GetResources()}
tex_info = {t.resourceId: t for t in ctl.GetTextures()}


def save(rid, name):
    t = tex_info.get(rid)
    if t is None:
        return None
    s = rd.TextureSave()
    s.resourceId = rid
    s.destType = rd.FileType.PNG
    s.alpha = rd.AlphaMapping.Discard
    path = os.path.join(out, name + ".png")
    ctl.SaveTexture(s, path)
    return {"w": t.width, "h": t.height, "fmt": str(t.format.Name()), "png": path}


for a in draws[-40:]:
    ctl.SetFrameEvent(a.eventId, True)
    st = ctl.GetPipelineState()
    entry = {"event": a.eventId, "name": a.GetName(ctl.GetStructuredFile()), "reads": [], "writes": []}
    try:
        ps = st.GetShaderReflection(rd.ShaderStage.Pixel)
        cs = st.GetShaderReflection(rd.ShaderStage.Compute)
        entry["shader"] = str((ps or cs).resourceId) if (ps or cs) else None
    except Exception as e:
        entry["shader_err"] = str(e)
    stage = rd.ShaderStage.Compute if a.flags & rd.ActionFlags.Dispatch else rd.ShaderStage.Pixel
    try:
        for i, ro in enumerate(st.GetReadOnlyResources(stage)):
            rid = ro.descriptor.resource
            if rid != rd.ResourceId.Null() and rid in tex_info:
                entry["reads"].append(save(rid, f"e{a.eventId}_r{i}"))
    except Exception as e:
        entry["reads_err"] = str(e)
    try:
        for i, o in enumerate(st.GetOutputTargets()):
            rid = o.resource
            if rid != rd.ResourceId.Null():
                entry["writes"].append(save(rid, f"e{a.eventId}_w{i}"))
    except Exception as e:
        entry["writes_err"] = str(e)
    report["tail"].append(entry)

with open(os.path.join(out, "report.json"), "w") as f:
    json.dump(report, f, indent=1, default=str)

ctl.Shutdown()
cap.Shutdown()
os._exit(0)
