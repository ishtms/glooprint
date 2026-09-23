"""Validate saved material fixtures in a disposable UE editor without GlooPrint.

Copy the NativeEditor .uasset/.expressions.csv/.asset.txt files to Content/Materials.
Enable PythonScriptPlugin and disable GlooPrint in that disposable project, then
run this file with -ExecutePythonScript. Does not save or modify source assets.
"""
import csv
import json
from pathlib import Path
import traceback

import unreal

project = Path(unreal.Paths.project_dir()).resolve()
results = {"status": "running", "assets": []}
library = unreal.MaterialEditingLibrary


def state(asset):
    is_function = isinstance(asset, unreal.MaterialFunction)
    expressions = (library.get_material_function_expressions(asset) if is_function
                   else library.get_material_expressions(asset))
    rows = {}
    for expression in expressions:
        x, y = library.get_material_expression_node_position(expression)
        inputs = (library.get_inputs_for_material_function_expression(asset, expression)
                  if is_function else library.get_inputs_for_material_expression(asset, expression))
        rows[expression.get_name()] = {
            "class": expression.get_class().get_path_name(), "position": [x, y],
            "inputs": [item.get_name() if item else "" for item in inputs],
        }
    return rows


try:
    assert not hasattr(unreal, "GlooPrintSettings"), "GlooPrint must be absent at startup"
    fixtures = sorted((project / "Content/Materials").glob("*.expressions.csv"))
    assert fixtures, "No material persistence fixtures found"
    for metadata in fixtures:
        stem = metadata.name.removesuffix(".expressions.csv")
        name = metadata.with_name(stem + ".asset.txt").read_text(encoding="utf-8-sig").strip()
        asset = unreal.load_asset(f"/Game/Materials/{stem}.{name}")
        assert asset is not None, f"Could not load {stem}"
        expected = {}
        with metadata.open(encoding="utf-8-sig", newline="") as file:
            for row in csv.DictReader(file):
                expected[row["name"]] = {"class": row["class"], "position": [int(row["x"]), int(row["y"])],
                                          "inputs": row["inputs"].split("|") if row["inputs"] else []}
        before = state(asset)
        # Native scripting omits unconnected input entries; compare connections
        # separately while C++ tests retain the complete serialized input arrays.
        for values in (before, expected):
            for row in values.values():
                row["inputs"] = [item for item in row["inputs"] if item]
        assert before == expected, f"Saved expressions or coordinates differ for {stem}"
        if isinstance(asset, unreal.MaterialFunction):
            library.update_material_function(asset)
        else:
            errors = library.recompile_material(asset)
            assert not errors, f"Material compile errors: {errors}"
        after = state(asset)
        for row in after.values():
            row["inputs"] = [item for item in row["inputs"] if item]
        assert after == before, f"Compilation changed the saved material graph for {stem}"
        results["assets"].append({"fixture": stem, "expressions": len(before), "status": "passed"})
    results["status"] = "passed"
except Exception:
    results["status"] = "failed"
    results["error"] = traceback.format_exc()
    raise
finally:
    (project / "material-verification.json").write_text(json.dumps(results, indent=2) + "\n")
