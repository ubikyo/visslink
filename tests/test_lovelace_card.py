"""Carte Lovelace des programmations (v0.8) : test headless avec Node."""

import re
import shutil
import subprocess

import pytest

CARD = "lovelace/visslink-schedule-card.js"


@pytest.mark.skipif(shutil.which("node") is None, reason="node absent")
def test_card_logic_and_rendering(repo_root):
    result = subprocess.run(
        ["node", str(repo_root / "tests/js/card_test.js")],
        check=False,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "card tests: ok" in result.stdout


def test_card_uses_only_public_home_assistant_apis(repo_root):
    """Aucun élément interne (ha-*), aucune ressource externe : un seul
    fichier, compatible avec les mises à jour du frontend."""
    text = (repo_root / CARD).read_text(encoding="utf-8")
    assert not re.search(r"<ha-[a-z-]+", text)
    assert "import " not in text
    assert "http://" not in text and "https://" not in text
    assert 'customElements.define("visslink-schedule-card"' in text
    # Même limite de fin de plage que l'ESP.
    assert "const VL_MAX_END = 23 * 60 + 50;" in text
    # Toutes les actions déclarées dans l'exemple, et seulement elles.
    used = set(re.findall(r'_(?:call|send)\(\s*"([a-z]+)"', text))
    assert used == {"read", "set", "copy", "refresh", "undo"}, used
