"""Validation de la configuration ESPHome (`esphome config`).

L'exemple est copié dans un dossier temporaire avec des secrets fictifs générés
à la volée : aucun identifiant n'est écrit dans le dépôt.
"""

import base64
import os
import re
import subprocess

import pytest

EXAMPLE = "examples/iot-viessmann.yaml"
# Exemple de la v0.1.0, tel que publié : doit rester valide sans modification.
EXAMPLE_V01 = "tests/fixtures/iot-viessmann-v0.1.yaml"


def _secrets() -> str:
    key = base64.b64encode(os.urandom(32)).decode()
    return (
        'wifi_ssid: "test"\n'
        'wifi_password: "test-password"\n'
        'fallback_ap_password: "test-fallback"\n'
        f'api_encryption_key: "{key}"\n'
    )


def _run_config(repo_root, tmp_path, transform=lambda s: s, example=EXAMPLE):
    text = (repo_root / example).read_text(encoding="utf-8")
    text = text.replace("path: ../components", f"path: {repo_root / 'components'}")
    text = transform(text)
    (tmp_path / "secrets.yaml").write_text(_secrets(), encoding="utf-8")
    config = tmp_path / "test.yaml"
    config.write_text(text, encoding="utf-8")
    return subprocess.run(
        ["esphome", "config", str(config)],
        check=False,
        capture_output=True,
        text=True,
        cwd=tmp_path,
    )


def test_example_config_is_valid(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "Configuration is valid" in result.stdout + result.stderr


def test_example_has_no_hardcoded_secrets(repo_root):
    text = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    for key in ("password:", "key:"):
        for line in text.splitlines():
            if line.strip().startswith(key):
                assert "!secret" in line, line


INVALID_CASES = {
    "wrong_baud_rate": lambda s: s.replace("baud_rate: 4800", "baud_rate: 9600"),
    "wrong_parity": lambda s: s.replace("parity: EVEN", "parity: NONE"),
    "wrong_stop_bits": lambda s: s.replace("stop_bits: 2", "stop_bits: 1"),
    # Option retirée : le capteur lit toujours un int16 de 2 octets.
    "unknown_length_option": lambda s: s.replace(
        "address: 0x0800", "address: 0x0800\n    length: 2"
    ),
    "interval_too_short": lambda s: s.replace(
        "update_interval: 60s", "update_interval: 500ms", 1
    ),
    "interval_too_long": lambda s: s.replace(
        "update_interval: 60s", "update_interval: 30d", 1
    ),
    "response_timeout_above_sync_period": lambda s: s.replace(
        "response_timeout: 1s", "response_timeout: 2s"
    ),
    "duplicate_register": lambda s: s.replace("address: 0x0804", "address: 0x0800"),
    # v0.2 : types de données et binary_sensor.
    "unknown_data_type": lambda s: s.replace(
        "data_type: int16", "data_type: float32", 1
    ),
    "signed_32_bit_not_supported": lambda s: s.replace(
        "data_type: uint32", "data_type: int32", 1
    ),
    "data_type_on_binary_sensor": lambda s: s.replace(
        "address: 0x0842", "address: 0x0842\n    data_type: uint8"
    ),
    "length_on_binary_sensor": lambda s: s.replace(
        "address: 0x0842", "address: 0x0842\n    length: 1"
    ),
    "binary_interval_too_short": lambda s: s.replace(
        "update_interval: 30s", "update_interval: 100ms"
    ),
    # Un registre n'est lu qu'une fois, toutes plateformes confondues.
    "duplicate_register_across_platforms": lambda s: s.replace(
        "address: 0x0842", "address: 0x0800"
    ),
    "address_out_of_range": lambda s: s.replace("address: 0x0804", "address: 0x10000"),
}


@pytest.mark.parametrize("case", sorted(INVALID_CASES))
def test_invalid_config_is_rejected(repo_root, tmp_path, case):
    result = _run_config(repo_root, tmp_path, INVALID_CASES[case])
    assert result.returncode != 0, f"{case} should be rejected:\n{result.stdout}"


# --- Compatibilité v0.1 --------------------------------------------------


def test_v01_config_still_valid_and_defaults_to_int16(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=EXAMPLE_V01)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "data_type" not in (repo_root / EXAMPLE_V01).read_text(encoding="utf-8")
    # Les trois capteurs de la v0.1 reçoivent le type par défaut.
    assert out.count("data_type: int16") == 3, out


def test_v01_fixture_matches_released_example(repo_root):
    text = (repo_root / EXAMPLE_V01).read_text(encoding="utf-8")
    assert text.startswith("# VissLink v0.1")


# --- Types de données ------------------------------------------------------


@pytest.mark.parametrize("data_type", ["uint8", "int16", "uint16", "uint32"])
def test_each_data_type_is_accepted(repo_root, tmp_path, data_type):
    result = _run_config(
        repo_root,
        tmp_path,
        lambda s: s.replace("data_type: int16", f"data_type: {data_type}", 1),
    )
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert f"data_type: {data_type}" in out


def test_data_type_is_case_insensitive(repo_root, tmp_path):
    result = _run_config(
        repo_root,
        tmp_path,
        lambda s: s.replace("data_type: uint32", "data_type: UINT32", 1),
    )
    assert result.returncode == 0, result.stdout + result.stderr


# --- Registres expérimentaux ----------------------------------------------

_COMMENTED = re.compile(r"^  # (- |  )", re.MULTILINE)
_RAW_0846 = re.compile(
    r'  # - platform: visslink\n  #   name: "Brut 0x0846[^\n]*\n(  #   [^\n]*\n)*'
)


def _enable_experimental(text):
    """Décommente les entités des sections sensor/binary_sensor uniquement."""
    head, sep, tail = text.partition("\nsensor:\n")
    return head + sep + _COMMENTED.sub(r"  \1", tail)


def test_example_keeps_experimental_registers_commented(repo_root):
    text = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    active = [line for line in text.splitlines() if not line.lstrip().startswith("#")]
    active = "\n".join(active)
    for address in ("0x5502", "0x0846", "0x0845", "0x0883", "0x2906", "0x3906"):
        assert f"address: {address}" not in active, address
        assert f"address: {address}" in text, address


def test_experimental_registers_are_valid_once_enabled(repo_root, tmp_path):
    def transform(text):
        text = _RAW_0846.sub("", text)  # étape 1 terminée : entrée brute retirée
        return _enable_experimental(text)

    assert "Brut 0x0846" not in transform(
        (repo_root / EXAMPLE).read_text(encoding="utf-8")
    )
    result = _run_config(repo_root, tmp_path, transform)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    # `esphome config` affiche les adresses sans zéro de tête (0x846).
    shown = {int(a, 16) for a in re.findall(r"address: (0x[0-9a-fA-F]+)", out)}
    for address in (0x5502, 0x0846, 0x0845, 0x0883, 0x2906, 0x3906):
        assert address in shown, hex(address)


def test_raw_check_and_binary_on_same_register_is_rejected(repo_root, tmp_path):
    # Vérification brute (uint8) et binary_sensor actifs ensemble : doublon.
    result = _run_config(repo_root, tmp_path, _enable_experimental)
    out = result.stdout + result.stderr
    assert result.returncode != 0, out
    assert "0x0846" in out, out
