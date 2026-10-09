import re, os
p = "../Butterscotch360.vcxproj"
s = open(p, "rb").read().decode("utf-8-sig")
items = re.findall(r'<ClCompile Include="([^"]+)"\s*(?:/>|>(.*?)</ClCompile>)', s, re.S)
os.makedirs("cxx", exist_ok=True)
srcs = []
seen = set()
for path, body in items:
    path = path.replace("\\", "/")
    if path in seen:
        continue
    seen.add(path)
    isc = "<CompileAs>CompileAsC</CompileAs>" in (body or "")
    if path.endswith(".cpp") or isc:
        srcs.append("../" + path)
    elif path.endswith(".c"):
        w = "cxx/" + path.replace("/", "__")[:-2] + ".cpp"
        open(w, "w").write('#include "../../%s"\n' % path)
        srcs.append(w)
    else:
        print("ignorado:", path)
for path in ["src/image/image_decoder.c"]:
    if "../" + path not in srcs:
        w = "cxx/" + path.replace("/", "__")[:-2] + ".cpp"
        open(w, "w").write('#include "../../%s"\n' % path)
        srcs.append(w)
with open("srcs.mk", "w") as f:
    f.write("SRCS := " + " \\\n  ".join(srcs) + "\n")
print(len(srcs), "arquivos")
