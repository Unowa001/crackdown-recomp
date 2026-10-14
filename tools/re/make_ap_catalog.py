"""Build the versioned AP catalog (metadata only) from a discovery export.

The output, crackdown/ap/catalog.json, is the single source of IDs for the C++
bridge (embedded at build time) and the Python world. Orb location ID =
location_base + save-array ordinal; the runtime re-verifies each selected
orb's type and position before trusting an ordinal.
Usage: make_ap_catalog.py out/ap-discovery/catalog-probe.json crackdown/ap/catalog.json
"""
import hashlib
import json
import argparse

VERSION = "crackdown-m1-dev-1"
LOCATION_BASE = 73_400_000  # dev namespace; public IDs not yet approved
ITEM_BASE = 73_410_000
# Developer world: the live-verified agility orb 178 and hidden orb 508, plus
# the nearest agility orbs around them (reachable from supply point 0).
DEV_ANCHORS = (178, 508)
DEV_EXTRA = 10


def main(src, dst, *, m2=False):
    raw = open(src, "rb").read()
    probe = json.loads(raw)
    orbs = probe["orbs"]
    assert len(orbs) == 800 and all(o["save_index_candidate"] == i for i, o in enumerate(orbs))
    names = ["Agility Orb" if o["type"] < 4 else "Hidden Orb" for o in orbs]
    ax, ay, az = orbs[178]["position"]
    near = sorted((i for i, o in enumerate(orbs) if o["type"] < 4 and i not in DEV_ANCHORS),
                  key=lambda i: sum((a - b) ** 2 for a, b in zip(orbs[i]["position"], (ax, ay, az))))
    dev = sorted(DEV_ANCHORS + tuple(near[:DEV_EXTRA]))
    catalog = {
        "version": VERSION,
        "game": "Crackdown",
        "supported_xex_sha256": probe["supported_xex_sha256"],
        "source": {"file": "out/ap-discovery/catalog-probe.json",
                   "sha256": hashlib.sha256(raw).hexdigest(), "schema": probe["schema"]},
        "location_base": LOCATION_BASE,
        "items": {"Progressive Agility": ITEM_BASE},
        "agility": {"starting": 1, "maximum": 5},
        "orbs": [{"id": LOCATION_BASE + i, "ordinal": i, "type": o["type"],
                  "name": f"{names[i]} {i:03d}",
                  "position": [round(v, 3) for v in o["position"]]} for i, o in enumerate(orbs)],
        "dev_world_orbs": dev,
    }
    if m2:
        catalog["version"] = "crackdown-m2-dev-1"
        skill_names = ("Agility", "Driving", "Explosives", "Strength", "Firearms")
        catalog["items"] = {f"Progressive {name}": ITEM_BASE + index
                            for index, name in enumerate(skill_names)}
        catalog["skills"] = [{"index": index, "name": name, "item": ITEM_BASE + index,
                              "starting": 1, "maximum": 5}
                             for index, name in enumerate(skill_names)]
        catalog["world_orbs"] = list(range(800))
        catalog["filler_item"] = ITEM_BASE + 1000
        catalog["items"]["Agency Commendation"] = catalog["filler_item"]
        # Native indices/IDs, never vector addresses or discovery order.
        catalog["supply_points"] = [{
            "id": LOCATION_BASE + 1000 + s["index_candidate"],
            "index": s["index_candidate"],
            "name": f"Secure Supply Point {s['index_candidate']:02d}",
            "position": [round(v, 3) for v in s["position"]],
            "starting": s["index_candidate"] == 15,
        } for s in probe["supply_points"]]
        kingpins = {3, 11, 17}
        catalog["bosses"] = [{
            "id": LOCATION_BASE + 2000 + b["id"], "native_id": b["id"],
            "name": f"Defeat {b['name']}", "kingpin": b["id"] in kingpins,
        } for b in probe["bosses"]]
        catalog["weapons"] = []
        for w in probe["armory_entries"]:
            native = w["native_id_candidate"]
            item = ITEM_BASE + 100 + native
            catalog["items"][w["name"]] = item
            catalog["weapons"].append({
                "id": LOCATION_BASE + 3000 + native,
                "native_id": native, "index": w["index_candidate"], "item": item,
                "name": w["name"], "location_name": f"Deposit {w['name']}",
                "starting": bool(w["starting_available"]),
            })
        catalog["reserved_items"] = {
            "Los Muertos Kingpin Access": ITEM_BASE + 2003,
            "Volk Kingpin Access": ITEM_BASE + 2011,
            "Shai-Gen Kingpin Access": ITEM_BASE + 2017,
        }
    with open(dst, "w", encoding="ascii", newline="\n") as f:
        json.dump(catalog, f, indent=1)
        f.write("\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("src")
    parser.add_argument("dst")
    parser.add_argument("--m2", action="store_true", help="Full M2 metadata contract")
    args = parser.parse_args()
    main(args.src, args.dst, m2=args.m2)
