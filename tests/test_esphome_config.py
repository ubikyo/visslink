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
# Exemples de la v0.1.0 et de la v0.2, tels que publiés : doivent rester
# valides sans modification.
EXAMPLE_V01 = "tests/fixtures/iot-viessmann-v0.1.yaml"
EXAMPLE_V02 = "tests/fixtures/iot-viessmann-v0.2.yaml"
# v0.2.2 : configuration de diagnostic (exemple + six registres TEST bruts).
DIAGNOSTIC = "examples/iot-viessmann-diagnostic.yaml"


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
    # v0.2.1 : programmations horaires (text_sensor).
    # Les 7 blocs comptent : 0x2008 est le mardi de 0x2000.
    "sensor_inside_schedule_blocks": lambda s: s.replace(
        "address: 0x0804", "address: 0x2008"
    ),
    "binary_sensor_on_schedule_sunday": lambda s: s.replace(
        "address: 0x0842", "address: 0x2230"
    ),
    "overlapping_schedules": lambda s: s.replace("address: 0x3000", "address: 0x2030"),
    "same_schedule_twice": lambda s: s.replace("address: 0x2100", "address: 0x2000"),
    "schedule_address_too_high": lambda s: s.replace(
        "address: 0x2200", "address: 0xFFD0"
    ),
    "schedule_interval_too_long": lambda s: s.replace(
        "update_interval: 6h", "update_interval: 8d", 1
    ),
    "today_and_active_without_time": lambda s: s.replace(
        "    time_id: ha_time\n", "", 1
    ),
    "active_without_time": lambda s: _drop_first(
        _drop_first(s, "    time_id: ha_time\n"),
        '    today:\n      name: "Radiateurs - Programmation du jour"\n',
    ),
    "data_type_on_schedule": lambda s: s.replace(
        "address: 0x2000", "address: 0x2000\n    data_type: uint8"
    ),
    "length_on_schedule": lambda s: s.replace(
        "address: 0x2000", "address: 0x2000\n    length: 8"
    ),
    "unknown_day_key": lambda s: s.replace(
        "address: 0x2000", 'address: 0x2000\n    lundi:\n      name: "Lundi"'
    ),
}


def _drop_first(text, fragment):
    assert fragment in text, fragment
    return text.replace(fragment, "", 1)


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


# --- Compatibilité v0.2 --------------------------------------------------


def test_v02_config_still_valid(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=EXAMPLE_V02)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    text = (repo_root / EXAMPLE_V02).read_text(encoding="utf-8")
    assert text.startswith("# VissLink v0.2 ")
    assert "text_sensor:" not in text


# --- Programmations horaires (v0.2.1) ----------------------------------------

SCHEDULES = (0x2000, 0x3000, 0x2100, 0x2200)


def test_example_declares_four_schedules(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    shown = {int(a, 16) for a in re.findall(r"address: (0x[0-9a-fA-F]+)", out)}
    for address in SCHEDULES:
        assert address in shown, hex(address)
    assert out.count("update_interval: 6h") == 4, out


def _minimal_schedule(text):
    """Remplace les programmations de l'exemple par une seule, minimale."""
    head, _, _ = text.partition("\ntext_sensor:\n")
    return (
        head
        + "\ntext_sensor:\n"
        + "  - platform: visslink\n"
        + '    name: "Radiateurs - Programmation"\n'
        + "    address: 0x2000\n"
    )


def test_minimal_schedule_defaults_to_6h_without_clock(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, _minimal_schedule)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "update_interval: 6h" in out, out


def test_schedule_day_sensors_are_accepted(repo_root, tmp_path):
    days = (
        "monday",
        "tuesday",
        "wednesday",
        "thursday",
        "friday",
        "saturday",
        "sunday",
    )

    def transform(text):
        text = _minimal_schedule(text)
        return text + "".join(
            f'    {day}:\n      name: "Radiateurs - {day}"\n' for day in days
        )

    result = _run_config(repo_root, tmp_path, transform)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for day in days:
        assert f"Radiateurs - {day}" in out, day


def test_schedule_last_valid_address(repo_root, tmp_path):
    # 0xFFC8 + 6 x 8 + 7 = 0xFFFF : le dimanche tient encore sur 16 bits.
    result = _run_config(
        repo_root,
        tmp_path,
        lambda s: s.replace("address: 0x2200", "address: 0xFFC8"),
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_schedule_next_to_another_register_is_accepted(repo_root, tmp_path):
    # 0x2038 suit immédiatement le dimanche de 0x2000 (0x2030-0x2037).
    result = _run_config(
        repo_root,
        tmp_path,
        lambda s: s.replace("address: 0x0804", "address: 0x2038"),
    )
    assert result.returncode == 0, result.stdout + result.stderr


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


# --- Configuration de diagnostic (v0.2.2) -------------------------------------

EXPERIMENTAL = {
    0x0846: "uint8",
    0x0845: "uint8",
    0x2906: "uint8",
    0x3906: "uint8",
    0x0883: "uint8",
    0x5502: "int16",
}
_TEST_ENTRY = re.compile(
    r'  - platform: visslink\n    name: "TEST [^\n]*\n(    [^\n]*\n)*'
)


def _active_lines(text):
    lines = (line for line in text.splitlines() if line.strip())
    return "\n".join(line for line in lines if not line.lstrip().startswith("#"))


def test_diagnostic_config_reads_experimental_registers_raw(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=DIAGNOSTIC)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    entries = re.findall(
        r"name: TEST [^\n]*\n((?:    [^\n]*\n)*)", out
    )  # sortie de `esphome config` : une entrée par capteur TEST
    assert len(entries) == len(EXPERIMENTAL), out
    seen = {}
    for entry in entries:
        address = int(re.search(r"address: (0x[0-9a-fA-F]+)", entry).group(1), 16)
        seen[address] = re.search(r"data_type: (\w+)", entry).group(1)
        assert "entity_category: diagnostic" in entry, entry
        assert "filters" not in entry and "unit_of_measurement" not in entry, entry
    assert seen == EXPERIMENTAL


def test_diagnostic_config_is_the_example_plus_test_sensors(repo_root):
    # Même nœud, mêmes Wi-Fi / API / OTA / UART / registres : seules les six
    # entrées TEST (et les commentaires) diffèrent.
    example = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    diagnostic = (repo_root / DIAGNOSTIC).read_text(encoding="utf-8")
    assert len(_TEST_ENTRY.findall(diagnostic)) == len(EXPERIMENTAL)
    assert _active_lines(_TEST_ENTRY.sub("", diagnostic)) == _active_lines(example)
