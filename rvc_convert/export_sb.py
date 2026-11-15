import sys, torch
sys.path.insert(0, "rvc")
from infer.lib.infer_pack.models_onnx import SynthesizerTrnMsNSFsidM

cpt = torch.load("SpongeBobSquarePants.pth", map_location="cpu", weights_only=False)
cpt["config"][-3] = cpt["weight"]["emb_g.weight"].shape[0]   # speaker count from weights (RVC export does this)
version = cpt.get("version", "v1")
vec = 256 if version == "v1" else 768
print("version", version, "vec", vec, "sr", cpt.get("sr"))

net_g = SynthesizerTrnMsNSFsidM(*cpt["config"], is_half=False, version=version)
net_g.load_state_dict(cpt["weight"], strict=False)
net_g.eval()

T = 200
args = (
    torch.rand(1, T, vec),
    torch.tensor([T]).long(),
    torch.randint(size=(1, T), low=5, high=255),
    torch.rand(1, T),
    torch.LongTensor([0]),
    torch.rand(1, 192, T),
)
kw = dict(
    dynamic_axes={"phone": [1], "pitch": [1], "pitchf": [1], "rnd": [2]},
    do_constant_folding=False, opset_version=18,
    input_names=["phone", "phone_lengths", "pitch", "pitchf", "ds", "rnd"],
    output_names=["audio"],
)
try:
    torch.onnx.export(net_g, args, "SpongeBob.onnx", dynamo=False, **kw)
except TypeError:
    torch.onnx.export(net_g, args, "SpongeBob.onnx", **kw)   # older torch w/o dynamo kwarg
print("EXPORT OK")
