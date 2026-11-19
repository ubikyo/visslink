"""Validation de la configuration ESPHome (`esphome config`).

L'exemple est copié dans un dossier temporaire avec des secrets fictifs générés
à la volée : aucun identifiant n'est écrit dans le dépôt.
"""

import ast
import base64
import os
import re
import subprocess

import pytest

EXAMPLE = "examples/iot-viessmann-v1.0.0.yaml"
GITHUB_SOURCE = "github://ubikyo/visslink@v1.0.0"
# Options d'autorisation supprimées en v1.0.0 : toutes les commandes sont
# directement utilisables (seules les protections techniques restent).
REMOVED_OPTIONS = (
    "enable_writes",
    "enable_mode_writes",
    "enable_m2_mode_write",
    "enable_schedule_writes",
    "enable_dhw_setpoint_write",
    "enable_heating_setpoint_writes",
    "allow_write",
    "validated_days",
)
# Exemples de la v0.1.0 et de la v0.2, tels que publiés : doivent rester
# valides sans modification.
EXAMPLE_V01 = "tests/fixtures/iot-viessmann-v0.1.yaml"
EXAMPLE_V02 = "tests/fixtures/iot-viessmann-v0.2.yaml"
# Exemple de production de la v0.2.2 (identique à la v0.2.1) et diagnostic,
# tels que publiés.
EXAMPLE_V022 = "tests/fixtures/iot-viessmann-v0.2.2.yaml"
DIAGNOSTIC_V022 = "tests/fixtures/iot-viessmann-v0.2.2-diagnostic.yaml"
# Exemple de la v0.3 (A1 en select, M2 en text_sensor), tel que publié.
EXAMPLE_V03 = "tests/fixtures/iot-viessmann-v0.3.yaml"
# Exemple de la v0.4 (deux select, trois autorisations), tel que publié.
EXAMPLE_V04 = "tests/fixtures/iot-viessmann-v0.4.yaml"
# Exemple de la v0.4.1, tel que publié.
EXAMPLE_V041 = "tests/fixtures/iot-viessmann-v0.4.1.yaml"


def _secrets() -> str:
    key = base64.b64encode(os.urandom(32)).decode()
    return (
        'wifi_ssid: "test"\n'
        'wifi_password: "test-password"\n'
        'fallback_ap_password: "test-fallback"\n'
        f'api_encryption_key: "{key}"\n'
    )


def _localize(repo_root, text):
    """Composants du dépôt au lieu de la version publiée ; fixtures : seule
    migration v1.0.0, les options d'autorisation `enable_*` sont retirées
    (comme la cible `compat` du Makefile)."""
    text = text.replace(
        f"  - source: {GITHUB_SOURCE}\n",
        f"  - source:\n      type: local\n      path: {repo_root / 'components'}\n",
    )
    text = text.replace("path: ../components", f"path: {repo_root / 'components'}")
    return re.sub(r"(?m)^  enable_[a-z0-9_]*: [^\n]*\n", "", text)


REMOVED_FLAGS_RE = re.compile(
    r"enable_(writes|mode_writes|m2_mode_write|schedule_writes|dhw_setpoint_write|"
    r"heating_setpoint_writes)"
)


def _without_editor(text):
    """Retire l'éditeur des programmations et ses actions API (leurs lambdas
    référencent `programmations`), pour tronquer l'exemple sans erreur d'ID."""
    head, sep, tail = text.partition("\n  actions:\n")
    assert sep
    text = head + "\n" + tail[tail.index("\nota:\n") + 1 :]
    head, sep, tail = text.partition("\nvisslink_schedule_editor:\n")
    assert sep
    return head + "\n" + tail[tail.index("\n# --- ") + 1 :]


def _run_config(repo_root, tmp_path, transform=lambda s: s, example=EXAMPLE):
    text = _localize(repo_root, (repo_root / example).read_text(encoding="utf-8"))
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
    # v0.3 : consignes (number).
    "number_address_not_whitelisted": lambda s: s.replace(
        "address: 0x2306", "address: 0x0846"
    ),
    "number_on_burner_register": lambda s: s.replace(
        "address: 0x2306", "address: 0x0842"
    ),
    "number_on_temperature_register": lambda s: s.replace(
        "address: 0x2306", "address: 0x0800"
    ),
    "same_setpoint_twice": lambda s: s.replace("address: 0x2307", "address: 0x2306"),
    "setpoint_also_declared_as_sensor": lambda s: s.replace(
        "address: 0x0804", "address: 0x6300"
    ),
    "number_min_below_documented": lambda s: s.replace(
        "min_value: 16", "min_value: 2", 1
    ),
    "number_max_above_documented": lambda s: s.replace(
        "max_value: 23", "max_value: 38", 1
    ),
    "dhw_max_above_documented": lambda s: s.replace("max_value: 60", "max_value: 61"),
    "dhw_min_below_documented": lambda s: s.replace("min_value: 40", "min_value: 9"),
    "number_min_not_below_max": lambda s: s.replace(
        "min_value: 16", "min_value: 24", 1
    ),
    "number_fractional_bound": lambda s: s.replace(
        "min_value: 16", "min_value: 16.5", 1
    ),
    "number_half_step": lambda s: s.replace("step: 1", "step: 0.5", 1),
    "number_step_two": lambda s: s.replace("step: 1", "step: 2", 1),
    "number_without_min_value": lambda s: s.replace("    min_value: 16\n", "", 1),
    "number_without_max_value": lambda s: s.replace("    max_value: 23\n", "", 1),
    "number_with_data_type": lambda s: s.replace(
        "address: 0x2306", "address: 0x2306\n    data_type: uint16"
    ),
    "number_with_restore_value": lambda s: s.replace(
        "address: 0x2306", "address: 0x2306\n    restore_value: true"
    ),
    "number_with_initial_value": lambda s: s.replace(
        "address: 0x2306", "address: 0x2306\n    initial_value: 20"
    ),
    # v0.3/v0.4 : modes de fonctionnement (deux select).
    "select_on_party_register": lambda s: s.replace(
        "address: 0x2301", "address: 0x2303"
    ),
    "select_on_candidate_2323": lambda s: s.replace(
        "address: 0x2301", "address: 0x2323"
    ),
    "select_on_candidate_3323": lambda s: s.replace(
        "address: 0x3301", "address: 0x3323"
    ),
    "select_on_setpoint": lambda s: s.replace("address: 0x2301", "address: 0x2307"),
    "select_on_schedule": lambda s: s.replace("address: 0x3301", "address: 0x2000"),
    "same_mode_register_twice": lambda s: s.replace(
        "address: 0x3301", "address: 0x2301"
    ),
    "mode_register_also_sensor": lambda s: s.replace(
        "address: 0x0804", "address: 0x3301"
    ),
    "mode_register_also_text_sensor": lambda s: _add_text_sensor(
        s,
        "  - platform: visslink\n    type: operating_mode\n"
        '    name: "M2 bis"\n    address: 0x3301\n',
    ),
    "mode_text_on_setpoint": lambda s: _add_text_sensor(
        s,
        "  - platform: visslink\n    type: operating_mode\n"
        '    name: "X"\n    address: 0x3306\n',
    ),
    "unknown_text_sensor_type": lambda s: _m2_as_text(s, type_="mode"),
    "mode_text_with_today": lambda s: _m2_as_text(
        s, extra='    today:\n      name: "Y"\n'
    ),
    "select_with_options": lambda s: s.replace(
        "address: 0x2301", 'address: 0x2301\n    options: ["Party"]'
    ),
    "select_with_data_type": lambda s: s.replace(
        "address: 0x3301", "address: 0x3301\n    data_type: uint8"
    ),
    "two_divergence_sensors": lambda s: s.replace(
        "    address: 0x3301\n",
        '    address: 0x3301\n    divergence:\n      name: "Autre"\n',
    ),
    "divergence_without_m2": lambda s: _drop_climate(_drop_first(s, _M2_SELECT)),
}


_M2_SELECT = (
    "  - platform: visslink\n"
    "    visslink_id: viessmann\n"
    "    id: m2_mode\n"
    '    name: "Plancher chauffant - Mode actuel"\n'
    "    address: 0x3301\n"
    "    update_interval: 5min\n"
)


def _add_text_sensor(text, entry):
    """Ajoute une entrée à la section text_sensor de l'exemple."""
    assert text.count("\ntext_sensor:\n") == 1
    return text.replace("\ntext_sensor:\n", "\ntext_sensor:\n" + entry, 1)


def _drop_climate(text):
    """Retire le climate M2 (il référence le select de M2)."""
    head, sep, tail = text.partition("\nclimate:\n")
    assert sep
    return head + "\n" + tail[tail.index("\n# --- ") + 1 :]


def _m2_as_text(text, type_="operating_mode", extra=""):
    """M2 déclaré en text_sensor (lecture seule) au lieu du select."""
    return _add_text_sensor(
        _drop_climate(_drop_first(text, _M2_SELECT)),
        f"  - platform: visslink\n    type: {type_}\n"
        f'    name: "M2"\n    address: 0x3301\n{extra}',
    )


def _drop_first(text, fragment):
    assert fragment in text, fragment
    return text.replace(fragment, "", 1)


@pytest.mark.parametrize("case", sorted(INVALID_CASES))
def test_invalid_config_is_rejected(repo_root, tmp_path, case):
    text = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    assert INVALID_CASES[case](text) != text, f"{case}: transformation sans effet"
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
    head, _, _ = _without_editor(text).partition("\ntext_sensor:\n")
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
        lambda s: _drop_first(s, "    - programmation_bouclage\n").replace(
            "address: 0x2200", "address: 0xFFC8"
        ),
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


# --- Configuration de diagnostic (v0.2.2) -------------------------------------


def _active_lines(text):
    lines = (line for line in text.splitlines() if line.strip())
    return "\n".join(line for line in lines if not line.lstrip().startswith("#"))


# --- Écritures et consignes (v0.3) --------------------------------------------

WRITABLE = {
    0x2306: (3, 37),
    0x2307: (3, 37),
    0x3306: (3, 37),
    0x3307: (3, 37),
    0x6300: (10, 60),
}
# v0.9.1 : liste blanche complète des consignes, avec les consignes Party.
PARTY_WRITABLE = {0x2308: (3, 37), 0x3308: (3, 37)}
WRITABLE_ALL = {**WRITABLE, **PARTY_WRITABLE}


def test_example_setpoints_are_numbers(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    _, _, numbers = out.partition("\nnumber:\n")
    numbers = numbers.partition("\nselect:\n")[0]
    shown = {
        int(a, 16): (int(lo), int(hi))
        for a, lo, hi in re.findall(
            r"address: (0x[0-9a-fA-F]+)\n    min_value: (\d+)\n    max_value: (\d+)",
            numbers,
        )
    }
    assert set(shown) == set(WRITABLE_ALL), numbers
    for address, (lo, hi) in shown.items():
        assert WRITABLE_ALL[address][0] <= lo < hi <= WRITABLE_ALL[address][1]
    assert numbers.count("step: 1") == 7, numbers
    # Les consignes ne sont pas déclarées en capteurs (adresse unique).
    sensors = _active_lines((repo_root / EXAMPLE).read_text(encoding="utf-8"))
    sensors = sensors.partition("\nsensor:")[2].partition("\nbinary_sensor:")[0]
    for address in WRITABLE_ALL:
        assert f"address: 0x{address:04X}" not in sensors


def test_target_number_syntax_is_accepted(repo_root, tmp_path):
    # API cible de la v0.3, telle que spécifiée.
    def transform(text):
        head, _, _ = _without_editor(text).partition("\nnumber:\n")
        return head + (
            "\nnumber:\n"
            "  - platform: visslink\n"
            "    visslink_id: viessmann\n"
            '    name: "Radiateurs - Consigne confort"\n'
            "    address: 0x2306\n"
            "    min_value: 16\n"
            "    max_value: 24\n"
            "    step: 1\n"
            '    unit_of_measurement: "°C"\n'
            "    mode: box\n"
        )

    result = _run_config(repo_root, tmp_path, transform)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("address", sorted(WRITABLE_ALL))
def test_documented_bounds_are_accepted(repo_root, tmp_path, address):
    lo, hi = WRITABLE_ALL[address]

    def transform(text):
        head, _, _ = _without_editor(text).partition("\nnumber:\n")
        return head + (
            "\nnumber:\n"
            "  - platform: visslink\n"
            '    name: "Consigne"\n'
            f"    address: 0x{address:04X}\n"
            f"    min_value: {lo}\n"
            f"    max_value: {hi}\n"
        )

    result = _run_config(repo_root, tmp_path, transform)
    assert result.returncode == 0, result.stdout + result.stderr


def test_python_and_cpp_whitelists_match(repo_root):
    cpp = (repo_root / "components/visslink/write_guard.h").read_text(encoding="utf-8")
    cpp_table = {
        int(a, 16): (int(lo), int(hi))
        for a, lo, hi in re.findall(r"\{0x([0-9A-Fa-f]{4}), 1, (\d+), (\d+)\}", cpp)
    }
    py = (repo_root / "components/visslink/number.py").read_text(encoding="utf-8")
    block = py.partition("WRITABLE_REGISTERS = {")[2].partition("}")[0]
    py_table = {
        int(a, 16): (int(lo), int(hi))
        for a, lo, hi in re.findall(r"0x([0-9A-Fa-f]{4}): \((\d+), (\d+)\)", block)
    }
    assert cpp_table == WRITABLE_ALL
    assert py_table == WRITABLE_ALL


# --- Compatibilité v0.2.1 / v0.2.2 ---------------------------------------------


@pytest.mark.parametrize("fixture", [EXAMPLE_V022, DIAGNOSTIC_V022])
def test_v022_configs_still_valid(repo_root, tmp_path, fixture):
    result = _run_config(repo_root, tmp_path, example=fixture)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    # v1.0.0 : plus aucune option d'autorisation ; la v0.2.2 n'a aucune
    # entité inscriptible (ni number ni select).
    assert not REMOVED_FLAGS_RE.search(out), out
    assert "\nnumber:" not in (repo_root / fixture).read_text(encoding="utf-8")


def test_cpp_writes_only_whitelisted_and_never_restored(repo_root):
    # Côté C++, aucun registre n'accepte d'écriture par défaut : seules les
    # entités des listes blanches le permettent. La plateforme number ne
    # restaure aucune valeur.
    header = (repo_root / "components/visslink/visslink.h").read_text(encoding="utf-8")
    assert "write_allowed(uint8_t /*value*/) const { return false; }" in header
    sources = header + (repo_root / "components/visslink/visslink.cpp").read_text(
        encoding="utf-8"
    )
    for forbidden in ("restore_value", "rtc_", "global_preferences", "load(&"):
        assert forbidden not in sources, forbidden
    number_py = (repo_root / "components/visslink/number.py").read_text(
        encoding="utf-8"
    )
    assert "CONF_RESTORE_VALUE" not in number_py
    assert "CONF_INITIAL_VALUE" not in number_py


# --- Programmes de fonctionnement (v0.3, expérimental) -----------------------

MODES = {0x00: "ECS seule", 0x03: "Chauffage + ECS", 0x05: "Arrêt"}


def test_example_mode_entities(repo_root, tmp_path):
    # Une entité par circuit, deux select, sans doublon numérique.
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    selects = out.partition("\nselect:\n")[2].partition("\ntext_sensor:\n")[0]
    shown = re.findall(r"name: ([^\n]+)\n    address: (0x[0-9a-fA-F]+)", selects)
    assert shown == [
        ("Radiateurs - Mode actuel", "0x2301"),
        ("Plancher chauffant - Mode actuel", "0x3301"),
    ], selects
    assert "type: operating_mode" not in out
    assert out.count("divergence:") == 1, out
    for address in ("0x2301", "0x3301"):
        assert out.count(f"address: {address}\n") == 1, address  # aucun doublon


def test_mode_text_sensors_still_valid(repo_root, tmp_path):
    # Lecture seule stricte possible pour les deux circuits (comme en v0.3).
    def transform(text):
        head, _, tail = text.partition("\nselect:\n")
        tail = tail[tail.index("\n# --- ") + 1 :]
        return _add_text_sensor(
            _drop_climate(head + "\n" + tail),
            "  - platform: visslink\n    type: operating_mode\n"
            '    name: "A1"\n    address: 0x2301\n'
            '    divergence:\n      name: "Div"\n'
            "  - platform: visslink\n    type: operating_mode\n"
            '    name: "M2"\n    address: 0x3301\n',
        )

    result = _run_config(repo_root, tmp_path, transform)
    assert result.returncode == 0, result.stdout + result.stderr


def test_mode_tables_match_cpp(repo_root):
    cpp = (repo_root / "components/visslink/write_guard.h").read_text(encoding="utf-8")
    values = re.search(r"OPERATING_MODE_VALUES\[\] = \{([^}]*)\}", cpp).group(1)
    readable = re.search(r"MODE_REGISTERS\[\] = \{([^}]*)\}", cpp).group(1)
    writable = re.search(r"MODE_WRITE_WHITELIST\[\] = \{([^}]*)\}", cpp).group(1)

    def as_ints(text):
        return [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]+", text)]

    assert as_ints(values) == list(MODES)
    assert as_ints(readable) == [0x2301, 0x3301]
    assert as_ints(writable) == [0x2301, 0x3301]
    assert "M2_MODE_REGISTER" not in cpp
    texts = re.findall(r'case 0x0([035]):\n\s+return "([^"]+)";', cpp)
    assert [(int(v), t) for v, t in texts] == list(MODES.items())
    assert 'return "Inconnu";' in cpp
    py = (repo_root / "components/visslink/__init__.py").read_text(encoding="utf-8")
    assert "MODE_REGISTERS = (0x2301, 0x3301)" in py
    assert "MODE_WRITE_WHITELIST = (0x2301, 0x3301)" in py
    assert "M2_MODE_REGISTER" not in py
    labels = re.findall(r'0x0([035]): "([^"]+)"', py)
    assert [(int(v), label) for v, label in labels] == list(MODES.items())
    for forbidden in ("Party", "Congé", "Ferien"):
        assert f'"{forbidden}"' not in py


def test_v03_config_still_valid(repo_root, tmp_path):
    # Exemple v0.3 inchangé (M2 en text_sensor) : valide, M2 non inscriptible.
    result = _run_config(repo_root, tmp_path, example=EXAMPLE_V03)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    text = (repo_root / EXAMPLE_V03).read_text(encoding="utf-8")
    assert text.startswith("# VissLink v0.3 ")
    assert not REMOVED_FLAGS_RE.search(out), out  # options retirées
    assert "type: operating_mode" in out


def test_m2_as_text_sensor_is_valid(repo_root, tmp_path):
    # Témoin des deux cas invalides ci-dessus : sans l'erreur injectée, la
    # même transformation est valide.
    result = _run_config(repo_root, tmp_path, _m2_as_text)
    assert result.returncode == 0, result.stdout + result.stderr


def test_v04_config_still_valid(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=EXAMPLE_V04)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    text = (repo_root / EXAMPLE_V04).read_text(encoding="utf-8")
    assert text.startswith("# VissLink v0.4 ")
    assert not REMOVED_FLAGS_RE.search(out), out  # options retirées


# --- Diagnostic bouclage ECS (lecture seule) ----------------------------------


# --- v0.5 : bouclage ECS à la demande -----------------------------------------


def test_v041_config_still_valid(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path, example=EXAMPLE_V041)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert (
        (repo_root / EXAMPLE_V041)
        .read_text(encoding="utf-8")
        .startswith("# VissLink v0.4.1 ")
    )
    assert not REMOVED_FLAGS_RE.search(out), out


def test_example_boost_available_every_day(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    boost = out.partition("visslink_boost:")[2].partition("\ntext_sensor:")[0]
    assert boost, out
    assert "validated_days" not in boost
    assert "ENABLED" not in out  # plus d'avertissement d'autorisation
    # Horloge lue en lecture seule, à 0x088E.
    assert re.search(
        r"name: Vitotronic - Horloge\n(?:    [^\n]*\n)*?    address: 0x88E\n", out
    ), out


@pytest.mark.parametrize(
    "case",
    [
        lambda s: s.replace(
            "\n  time_id: ha_time\n",
            "\n  time_id: ha_time\n  validated_days: [dimanche]\n",
            1,
        ),
        lambda s: s.replace("  time_id: ha_time\n  start:", "  start:"),
        lambda s: _drop_first(
            s, '  start:\n    name: "Bouclage ECS — Démarrer maintenant"\n'
        ),
        lambda s: s.replace(
            "type: vito_clock\n", "type: vito_clock\n    address: 0x0846\n"
        ),
        lambda s: s.replace(
            '  start:\n    name: "Bouclage ECS — Démarrer maintenant"\n',
            '  start:\n    name: "Bouclage ECS — Démarrer maintenant"\n  address: 0x0846\n',
        ),
    ],
)
def test_boost_invalid_configs(repo_root, tmp_path, case):
    result = _run_config(repo_root, tmp_path, case)
    assert result.returncode != 0, result.stdout


def test_boost_tables_match_cpp(repo_root):
    cpp = (repo_root / "components/visslink/circulation_boost.h").read_text(
        encoding="utf-8"
    )
    assert "LOOP_SCHEDULE_MONDAY = 0x2200;" in cpp
    durations = re.search(r"BOOST_DURATIONS\[\] = \{([^}]*)\}", cpp).group(1)
    py = (repo_root / "components/visslink_boost/__init__.py").read_text(
        encoding="utf-8"
    )
    options = re.search(r"DURATION_OPTIONS = \[([^\]]*)\]", py).group(1)
    assert [int(d) for d in re.findall(r"\d+", durations)] == [
        int(d) for d in re.findall(r"\d+", options)
    ]
    # Jamais d'écriture sur 0x0846 : aucune occurrence dans le code d'écriture
    # (hors commentaires).
    for f in ("circulation_boost.h", "write_guard.h"):
        text = (repo_root / "components/visslink" / f).read_text(encoding="utf-8")
        code = re.sub(r"//[^\n]*", "", text)
        assert "0x0846" not in code and "0x846" not in code, f


# --- v0.5 : états Éco et Party (lecture seule) --------------------------------

ECO_PARTY = {
    0x2302: "Radiateurs - Éco",
    0x2303: "Radiateurs - Party",
    0x3302: "Plancher chauffant - Éco",
    0x3303: "Plancher chauffant - Party",
}


@pytest.mark.parametrize("address", sorted(ECO_PARTY))
def test_eco_party_never_writable(repo_root, tmp_path, address):
    # Ni consigne (number) ni programme (select) sur ces adresses : refusé par
    # la liste blanche, pas seulement comme doublon.
    for old, expected in (
        ("address: 0x2306", "not in the VissLink write whitelist"),
        ("address: 0x2301", "is not an operating mode register"),
    ):
        result = _run_config(
            repo_root,
            tmp_path,
            lambda s, old=old: s.replace(old, f"address: 0x{address:04X}"),
        )
        out = result.stdout + result.stderr
        assert result.returncode != 0, hex(address)
        assert expected in out, out


def test_eco_party_duplicate_rejected(repo_root, tmp_path):
    result = _run_config(
        repo_root, tmp_path, lambda s: s.replace("address: 0x3303", "address: 0x3302")
    )
    assert result.returncode != 0, result.stdout


# --- Essai en lecture seule v0.5 ----------------------------------------------


# --- v0.5.1 : valeurs « non disponibles » (invalid_values) ------------------


def _flow_setpoint_sensor(extra, data_type="int16", address="0x2544"):
    def transform(text):
        sensor = (
            "sensor:\n"
            "  - platform: visslink\n"
            '    name: "TEST consigne départ"\n'
            f"    address: {address}\n"
            f"    data_type: {data_type}\n"
            f"{extra}"
            "    filters:\n"
            "      - multiply: 0.1\n"
        )
        assert text.count("\nsensor:\n") == 1
        return text.replace("\nsensor:\n", "\n" + sensor, 1)

    return transform


@pytest.mark.parametrize(
    "extra, data_type",
    [
        ("    invalid_values: 0xFFFF\n", "int16"),
        ("    invalid_values: [0xFFFF, 0x7FFF]\n", "int16"),
        ("    invalid_values: [0xFF]\n", "uint8"),
        ("    invalid_values: [0xFFFFFFFF]\n", "uint32"),
    ],
)
def test_invalid_values_accepted(repo_root, tmp_path, extra, data_type):
    result = _run_config(repo_root, tmp_path, _flow_setpoint_sensor(extra, data_type))
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "invalid_values" in out


@pytest.mark.parametrize(
    "extra, data_type, message",
    [
        ("    invalid_values: [0x10000]\n", "int16", "does not fit in int16"),
        ("    invalid_values: [0x100]\n", "uint8", "does not fit in uint8"),
        ("    invalid_values: [0xFFFF, 0xFFFF]\n", "int16", "duplicate value"),
        ("    invalid_values: [1, 2, 3, 4, 5]\n", "int16", "at most 4"),
        ("    invalid_values: []\n", "int16", "at least 1"),
    ],
)
def test_invalid_values_rejected(repo_root, tmp_path, extra, data_type, message):
    result = _run_config(repo_root, tmp_path, _flow_setpoint_sensor(extra, data_type))
    out = result.stdout + result.stderr
    assert result.returncode != 0, out
    assert message in out, out


def test_invalid_values_opt_in_only(repo_root):
    """Aucune valeur n'est invalide par défaut : FF FF reste -0,1 °C sur la
    température extérieure (0x0800), et l'option n'existe que pour `sensor`."""
    sensor_py = (repo_root / "components/visslink/sensor.py").read_text(
        encoding="utf-8"
    )
    assert "cv.Optional(CONF_INVALID_VALUES)" in sensor_py
    assert "CONF_INVALID_VALUES, default" not in sensor_py
    for name in ("binary_sensor.py", "number.py", "select.py", "text_sensor.py"):
        text = (repo_root / "components/visslink" / name).read_text(encoding="utf-8")
        assert "invalid_values" not in text


# --- v0.5.1 : décisions de configuration --------------------------------------

_ECO_PARTY = ("0x2302", "0x2303", "0x3302", "0x3303")


def _interval_after(text, address):
    match = re.search(
        rf"^    address: {address}\n    update_interval: (\S+)$", text, re.MULTILINE
    )
    assert match, address
    return match.group(1)


def test_eco_party_read_every_60s(repo_root):
    text = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    for address in _ECO_PARTY:
        assert _interval_after(text, address) == "60s", address


def test_no_write_path_for_loop_pump_0x0846(repo_root):
    """0x0846 n'est inscriptible par aucun chemin : listes blanches C++ et
    Python (consignes, programmes), blocs horaires du bouclage (0x2200..0x2237
    seulement, testé en C++). Seule mention permise : une phrase de
    documentation rappelant qu'il n'est jamais commandé."""
    root = repo_root / "components"
    for path in sorted(root.rglob("*")):
        if path.suffix not in (".py", ".h", ".cpp") or "__pycache__" in path.parts:
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            if "0846" in line:
                assert "jamais" in line, f"{path}: {line}"


# --- v0.5.2 : configuration d'essai matériel du bouclage --------------------


def test_loop_pump_0x0846_never_writable_in_examples(repo_root):
    """Aucune entité inscriptible (number, select) sur 0x0846 dans les
    exemples : seulement sensor / binary_sensor."""
    for path in sorted((repo_root / "examples").glob("*.yaml")):
        if path.name == "secrets.yaml":
            continue
        text = _active_lines(path.read_text(encoding="utf-8"))
        section = None
        for line in text.splitlines():
            if not line.startswith(" "):
                section = line.rstrip(":")
            if "address: 0x0846" in line:
                assert section in ("sensor", "binary_sensor"), (path, section)


def test_boost_default_duration_is_10_min(repo_root):
    """Durée proposée après chaque redémarrage de l'ESP : 10 min."""
    init = (repo_root / "components/visslink_boost/__init__.py").read_text(
        encoding="utf-8"
    )
    options = re.search(r"DURATION_OPTIONS = (\[[^\]]*\])", init).group(1)
    default = re.search(r'DEFAULT_DURATION = "([^"]*)"', init).group(1)
    assert default == "10 min"
    header = (repo_root / "components/visslink/circulation_boost.h").read_text(
        encoding="utf-8"
    )
    index = int(re.search(r"BOOST_DEFAULT_DURATION_INDEX = (\d+);", header).group(1))
    assert ast.literal_eval(options)[index] == default
    cpp = (repo_root / "components/visslink_boost/visslink_boost.cpp").read_text(
        encoding="utf-8"
    )
    assert "publish_state(visslink::BOOST_DEFAULT_DURATION_INDEX)" in cpp


def test_boost_restoring_entity_is_optional(repo_root, tmp_path):
    # L'exemple la déclare ; la retirer le laisse valide.
    def drop(text):
        old = '  restoring:\n    name: "Bouclage ECS — Restauration en cours"\n'
        assert old in text
        return text.replace(old, "")

    result = _run_config(repo_root, tmp_path, drop)
    assert result.returncode == 0, result.stdout + result.stderr


# --- v0.6 : consigne d'eau chaude sanitaire ---------------------------------


def test_dhw_limits_and_whitelist_match_cpp(repo_root):
    header = (repo_root / "components/visslink/write_guard.h").read_text(
        encoding="utf-8"
    )
    low = int(re.search(r"DHW_WRITE_MIN = (\d+);", header).group(1))
    high = int(re.search(r"DHW_WRITE_MAX = (\d+);", header).group(1))
    number_py = (repo_root / "components/visslink/number.py").read_text(
        encoding="utf-8"
    )
    limits = re.search(r"DHW_WRITE_LIMITS = \((\d+), (\d+)\)", number_py)
    assert (low, high) == (int(limits.group(1)), int(limits.group(2))) == (50, 60)
    assert "DHW_SETPOINT_ADDRESS = 0x6300" in number_py
    whitelist = re.search(
        r"DHW_SETPOINT_WRITE_WHITELIST\[\] = \{([^}]*)\}", header
    ).group(1)
    assert whitelist.strip() == "DHW_SETPOINT_ADDRESS"
    assert "static constexpr uint16_t DHW_SETPOINT_ADDRESS = 0x6300;" in header
    # Dans la plage documentée de la liste blanche générale.
    assert WRITABLE[0x6300][0] <= low and high <= WRITABLE[0x6300][1]


# --- v0.7 : consignes de chauffage A1/M2 et climate du plancher M2 -----------

HEATING = {0x2306: (16, 23), 0x2307: (10, 20), 0x3306: (16, 22), 0x3307: (14, 20)}


def test_heating_limits_match_cpp(repo_root):
    header = (repo_root / "components/visslink/write_guard.h").read_text(
        encoding="utf-8"
    )
    cpp = {
        int(a, 16): (int(lo), int(hi))
        for a, lo, hi in re.findall(
            r"\{0x([0-9A-Fa-f]{4}), (\d+), (\d+), 0x[0-9A-Fa-f]{4}, (?:true|false)\}",
            header,
        )
    }
    number_py = (repo_root / "components/visslink/number.py").read_text(
        encoding="utf-8"
    )
    block = number_py.partition("HEATING_WRITE_LIMITS = {")[2].partition("}")[0]
    py = {
        int(a, 16): (int(lo), int(hi))
        for a, lo, hi in re.findall(r"0x([0-9A-Fa-f]{4}): \((\d+), (\d+)\)", block)
    }
    assert cpp == py == HEATING
    # Pas les mêmes limites pour les radiateurs et le plancher.
    assert HEATING[0x2306] != HEATING[0x3306]
    assert HEATING[0x2307] != HEATING[0x3307]
    for address, (lo, hi) in HEATING.items():  # dans la plage documentée
        assert WRITABLE[address][0] <= lo < hi <= WRITABLE[address][1]


def _climate_transform(**changes):
    def transform(text):
        for old, new in changes.items():
            assert old in text, old
            text = text.replace(old, new, 1)
        return text

    return transform


@pytest.mark.parametrize(
    "old, new, message",
    [
        (
            "comfort_setpoint_id: m2_comfort_setpoint",
            "comfort_setpoint_id: a1_comfort",
            "comfort_setpoint_id must be the visslink number of the M2 comfort",
        ),
        (
            "mode_id: m2_mode",
            "mode_id: a1_mode",
            "mode_id must be the visslink select of the M2 operating mode",
        ),
        (
            "current_temperature_id: m2_room_temperature",
            "current_temperature_id: m2_flow",
            "current_temperature_id must be a room temperature, not 0x3900",
        ),
        (
            "burner_id: bruleur_actif",
            "burner_id: m2_pump",
            "burner_id must be the visslink binary_sensor of the burner (0x0842)",
        ),
        (
            "pump_id: m2_pump",
            "pump_id: bruleur_actif",
            "pump_id must be the visslink binary_sensor of the M2 circulation",
        ),
    ],
)
def test_climate_rejects_wrong_sources(repo_root, tmp_path, old, new, message):
    def transform(text):
        # Identifiants supplémentaires sur des entités existantes.
        text = text.replace(
            '    name: "Radiateurs - Consigne confort"\n',
            '    id: a1_comfort\n    name: "Radiateurs - Consigne confort"\n',
            1,
        )
        text = text.replace(
            '    name: "Radiateurs - Mode actuel"\n',
            '    id: a1_mode\n    name: "Radiateurs - Mode actuel"\n',
            1,
        )
        text = text.replace(
            '    name: "Température départ M2"\n',
            '    id: m2_flow\n    name: "Température départ M2"\n',
            1,
        )
        assert old in text
        return text.replace(old, new, 1)

    result = _run_config(repo_root, tmp_path, transform, example=EXAMPLE)
    out = result.stdout + result.stderr
    assert result.returncode != 0, out
    assert message in out, out


def test_climate_optional_sources(repo_root, tmp_path):
    def transform(text):
        for line in (
            "    current_temperature_id: m2_room_temperature\n",
            "    pump_id: m2_pump\n",
            "    burner_id: bruleur_actif\n",
            '    status:\n      name: "Plancher chauffant - État"\n',
        ):
            assert line in text
            text = text.replace(line, "", 1)
        return text

    result = _run_config(repo_root, tmp_path, transform, example=EXAMPLE)
    assert result.returncode == 0, result.stdout + result.stderr


def test_climate_m2_state_from_existing_reads(repo_root, tmp_path):
    """v0.7.3 : état du plancher à partir des lectures existantes (0x3301,
    0x3906, 0x0842), sans lecture supplémentaire."""
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "burner_id: bruleur_actif" in out, out
    assert "name: Plancher chauffant - État" in out, out
    for address in ("0x3301", "0x3906", "0x842"):
        assert out.count(f"address: {address}\n") == 1, address  # une seule lecture


def test_climate_burner_must_be_a_visslink_binary_sensor(repo_root, tmp_path):
    def transform(text):
        old = "    burner_id: bruleur_actif\n"
        assert old in text
        text = text.replace(old, "    burner_id: other_burner\n", 1)
        return text.replace(
            "binary_sensor:\n",
            "binary_sensor:\n  - platform: template\n    id: other_burner\n"
            '    name: "Autre"\n',
            1,
        )

    result = _run_config(repo_root, tmp_path, transform, example=EXAMPLE)
    assert result.returncode != 0, result.stdout + result.stderr


def test_climate_m2_state_documented(repo_root):
    """Les quatre états et la péremption figurent dans le code et la doc."""
    header = (repo_root / "components/visslink/circuit_climate.h").read_text(
        encoding="utf-8"
    )
    for text in ('"Chauffe"', '"Inertie"', '"Arrêt"', '"Inconnu"'):
        assert text in header, text
    assert "STALE_INTERVALS = 3" in header
    assert "STALE_MARGIN_MS = 30000" in header
    doc = (repo_root / "docs/consignes-chauffage-v0.7.md").read_text(encoding="utf-8")
    for text in ("Chauffe", "Inertie", "Arrêt", "Inconnu", "burner_id", "30 s"):
        assert text in doc, text


# --- v0.7.1 : configuration explicite « toutes écritures autorisées » --------


def test_boost_range_and_remaining_are_optional_display(repo_root, tmp_path):
    # Le temps restant est en minutes, la plage un texte ; tous deux
    # facultatifs.
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert re.search(
        r"remaining:\n(?:    [^\n]*\n)*?    unit_of_measurement: min\n", out
    ), out
    init = (repo_root / "components/visslink_boost/__init__.py").read_text(
        encoding="utf-8"
    )
    assert "cv.Optional(CONF_RANGE)" in init
    assert "cv.Optional(CONF_REMAINING)" in init


# --- v0.8 : éditeur des programmations ------------------------------------------

EDITOR_ACTIONS = (
    "visslink_schedule_read",
    "visslink_schedule_set",
    "visslink_schedule_copy",
    "visslink_schedule_clear",
    "visslink_schedule_refresh",
    "visslink_schedule_undo",
)


def test_schedule_editor_example_valid(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for action in EDITOR_ACTIONS:
        assert f"action: {action}\n" in out, action
    assert "supports_response: optional" in out  # lecture : réponse JSON
    assert out.count("supports_response: status") == 5  # succès / motif du refus
    assert "name: Programmations - État de l'édition" in out
    # Une seule lecture de chaque programmation (aucune lecture en plus).
    for address in ("0x2000", "0x3000", "0x2100", "0x2200"):
        assert out.count(f"address: {address}\n") == 1, address


@pytest.mark.parametrize(
    "transform, message",
    [
        (
            lambda t: t.replace(
                "    - programmation_bouclage\n", "    - programmation_a1\n", 1
            ),
            "schedules: 0x2000 declared twice",
        ),
        (
            lambda t: t.replace(
                'id: programmation_bouclage\n    name: "Bouclage ECS - Programmation"\n'
                "    address: 0x2200\n",
                'id: programmation_bouclage\n    name: "Bouclage ECS - Programmation"\n'
                "    address: 0x2400\n",
                1,
            ),
            "schedules: must be visslink schedule text_sensors",
        ),
    ],
)
def test_schedule_editor_rejects_wrong_schedules(
    repo_root, tmp_path, transform, message
):
    result = _run_config(repo_root, tmp_path, transform)
    out = result.stdout + result.stderr
    assert result.returncode != 0, out
    assert message in out, out


def test_schedule_editor_circuits_match_cpp(repo_root):
    """Mêmes circuits modifiables côté validation (Python) et côté écriture
    (liste blanche C++), et rien d'autre."""
    header = (repo_root / "components/visslink/schedule_editor.h").read_text(
        encoding="utf-8"
    )
    cpp = re.search(
        r"EDITOR_CIRCUIT_ADDRESSES\[EDITOR_CIRCUIT_COUNT\] = \{([^}]*)\}", header
    )
    assert cpp is not None
    addresses = [int(a, 16) for a in re.findall(r"0x([0-9A-Fa-f]{4})", cpp.group(1))]
    init = (repo_root / "components/visslink_schedule_editor/__init__.py").read_text(
        encoding="utf-8"
    )
    py = [
        int(a, 16) for a in re.findall(r"^    0x([0-9A-Fa-f]{4}):", init, re.MULTILINE)
    ]
    assert addresses == py == [0x2000, 0x3000, 0x2100, 0x2200]
    assert "EDITOR_LAST_END_MIN = 23 * 60 + 50" in header


def test_schedule_editor_has_no_permission_options(repo_root):
    """v1.0.0 : aucune option d'autorisation dans le composant de l'éditeur."""
    init = (repo_root / "components/visslink_schedule_editor/__init__.py").read_text(
        encoding="utf-8"
    )
    assert "enable_" not in init.lower()
    assert "validated_days" not in init


# --- v0.8.1 : diagnostics et cycles du brûleur -----------------------------------


def test_example_burner_cycles_without_extra_reads(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    # Registres réutilisés : chacun lu une seule fois, par son entité.
    # (esphome config affiche 0x842 pour 0x0842.)
    for address in ("0x842", "0x88A", "0x8A7", "0x802", "0x5502"):
        assert out.count(f"address: {address}\n") == 1, address
    for name in (
        "Brûleur - Démarrages sur 24 h",
        "Brûleur - Démarrages par heure",
        "Brûleur - Durée moyenne de fonctionnement",
        "Brûleur - Durée du dernier cycle (estimation)",
        "Brûleur - Durée du dernier arrêt (estimation)",
        "Brûleur - Température chaudière au démarrage (estimation)",
        "Brûleur - Température chaudière à l'arrêt (estimation)",
        "Brûleur - Précision des cycles",
        "Consigne chaudière calculée",
        "VissLink - Uptime",
        "VissLink - Signal Wi-Fi",
        "VissLink - Adresse IP",
        "Programmations - État de l'édition",
        "Bouclage ECS — État de l'opération",
    ):
        assert f"name: {name}\n" in out, name


@pytest.mark.parametrize(
    "transform, message",
    [
        (
            lambda t: t.replace(
                "\n  burner_id: bruleur_actif\n", "\n  burner_id: m2_pump\n"
            ),
            "burner_id must be the visslink binary_sensor of the burner (0x0842)",
        ),
        (
            lambda t: t.replace(
                "  starts_id: demarrages_bruleur\n",
                "  starts_id: temperature_chaudiere\n",
            ),
            "starts_id must be the visslink sensor of the burner starts counter",
        ),
        (
            lambda t: t.replace(
                "    address: 0x08A7\n    data_type: uint32",
                "    address: 0x08A7\n    data_type: uint16",
            ),
            "runtime_id must be the visslink sensor of the burner runtime counter",
        ),
        (
            lambda t: t.replace("  runtime_id: fonctionnement_bruleur\n", ""),
            "'average_runtime' needs 'runtime_id'",
        ),
        (
            lambda t: t.replace("  boiler_temperature_id: temperature_chaudiere\n", ""),
            "'start_temperature' needs 'boiler_temperature_id'",
        ),
    ],
)
def test_burner_cycles_rejects_wrong_sources(repo_root, tmp_path, transform, message):
    text = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    assert transform(text) != text
    result = _run_config(repo_root, tmp_path, transform)
    out = result.stdout + result.stderr
    assert result.returncode != 0, out
    assert message in out, out


def test_burner_cycles_reads_nothing(repo_root):
    """Le composant n'émet aucune lecture ni écriture Optolink."""
    src = "".join(
        (repo_root / "components/visslink_burner" / name).read_text(encoding="utf-8")
        for name in ("visslink_burner.h", "visslink_burner.cpp")
    )
    src += (repo_root / "components/visslink/burner_cycles.h").read_text(
        encoding="utf-8"
    )
    for forbidden in (
        "request_direct_read",
        "submit_block_write",
        "request_read",
        "request_write",
        "expedite",
        "submit(",
    ):
        assert forbidden not in src, forbidden


def test_boost_duration_select_goes_through_the_component(repo_root):
    """v0.8.1 : le choix de durée passe par le composant (raccourcie, ou
    refusée et ramenée à la durée en cours pendant une opération)."""
    init = (repo_root / "components/visslink_boost/__init__.py").read_text(
        encoding="utf-8"
    )
    assert "cg.add(sel.set_parent(var))" in init
    cpp = (repo_root / "components/visslink_boost/visslink_boost.cpp").read_text(
        encoding="utf-8"
    )
    assert "void BoostDurationSelect::control(size_t index) {" in cpp
    assert "this->parent_->duration_selected(index);" in cpp
    assert "this->sequencer_.change_duration(minutes, now)" in cpp


# --- Éco et Party ----------------------------------------------------------------

PARTY_SETPOINTS = (0x2308, 0x3308)


@pytest.mark.parametrize("address", PARTY_SETPOINTS)
def test_party_setpoints_never_operating_modes(repo_root, tmp_path, address):
    """Consigne Party : jamais un programme (select). v0.9.1 : number
    autorisé (liste blanche des consignes, cf. tests v0.9.1)."""
    result = _run_config(
        repo_root,
        tmp_path,
        lambda s: s.replace("address: 0x2301", f"address: 0x{address:04X}"),
    )
    out = result.stdout + result.stderr
    assert result.returncode != 0, hex(address)
    assert "is not an operating mode register" in out, out


def test_climate_has_no_eco_party_preset(repo_root):
    """Climate M2 conservé tel quel : HEAT/OFF, sans preset Éco/Party."""
    for path in (
        "components/visslink/climate.py",
        "components/visslink/visslink.cpp",
        "components/visslink/visslink.h",
        "components/visslink/circuit_climate.h",
    ):
        text = (repo_root / path).read_text(encoding="utf-8")
        assert "preset" not in text.lower(), path


def test_eco_party_registers_absent_from_other_cpp_whitelists(repo_root):
    """v0.9.1 : Éco, Party et consignes Party n'apparaissent dans aucune liste
    blanche du bouclage ni de l'éditeur ; dans write_guard.h, seulement dans
    leurs tables propres (et WRITABLE_REGISTERS pour les consignes Party)."""
    addresses = ("0x2302", "0x2303", "0x3302", "0x3303", "0x2308", "0x3308")
    code = ""
    for path in (
        "components/visslink/circulation_boost.h",
        "components/visslink/schedule_editor.h",
    ):
        text = (repo_root / path).read_text(encoding="utf-8")
        code += re.sub(r"//[^\n]*", "", text).upper()
    for address in addresses:
        assert address.upper() not in code, address
    guard = (repo_root / "components/visslink/write_guard.h").read_text(
        encoding="utf-8"
    )
    for table in (
        "MODE_REGISTERS[]",
        "MODE_WRITE_WHITELIST[]",
        "HEATING_SETPOINTS[]",
        "DHW_SETPOINT_WRITE_WHITELIST[]",
    ):
        body = guard.partition(table)[2].partition("};")[0].upper()
        assert body, table
        for address in addresses:
            assert address.upper() not in body, (table, address)


# --- Pilotage Éco / Party -------------------------------------------------------

ECO_PARTY_FLAGS = {
    0x2302: ("A1", "Eco"),
    0x2303: ("A1", "Party"),
    0x3302: ("M2", "Eco"),
    0x3303: ("M2", "Party"),
}
PARTY_LIMITS = {0x2308: (16, 23), 0x3308: (16, 22)}


def _switch(address, extra=""):
    return (
        "\nswitch:\n"
        "  - platform: visslink\n"
        "    visslink_id: viessmann\n"
        '    name: "Commande"\n'
        f"    address: 0x{address:04X}\n" + extra
    )


def _with_switch(text, block):
    """Remplace la section switch de l'exemple par `block`."""
    head, sep, rest = text.partition("\nswitch:\n")
    assert sep
    tail = rest[rest.index("\n# --- ") :]
    return head + block + tail


@pytest.mark.parametrize("address", sorted(ECO_PARTY_FLAGS))
def test_eco_party_switch_accepted_only_on_the_four_commands(
    repo_root, tmp_path, address
):
    def transform(text):
        return _with_switch(text, _switch(address))

    result = _run_config(repo_root, tmp_path, transform)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "restore_mode: DISABLED" in out


@pytest.mark.parametrize(
    "address", [0x2301, 0x2304, 0x2306, 0x2308, 0x3301, 0x6300, 0x0842, 0x2200]
)
def test_eco_party_switch_rejects_other_registers(repo_root, tmp_path, address):
    def transform(text):
        return _with_switch(text, _switch(address))

    result = _run_config(repo_root, tmp_path, transform)
    out = result.stdout + result.stderr
    assert result.returncode != 0, hex(address)
    assert "is not an Eco/Party command" in out, out


@pytest.mark.parametrize(
    "extra, expected",
    [
        ("    restore_mode: ALWAYS_OFF\n", "restore_mode must be DISABLED"),
        ("    restore_mode: RESTORE_DEFAULT_ON\n", "restore_mode must be DISABLED"),
        ("    inverted: true\n", "inverted"),
    ],
)
def test_eco_party_switch_never_commands_at_boot(repo_root, tmp_path, extra, expected):
    def transform(text):
        return _with_switch(text, _switch(0x2303, extra))

    result = _run_config(repo_root, tmp_path, transform)
    out = result.stdout + result.stderr
    assert result.returncode != 0, extra
    assert expected in out, out


def test_eco_party_switch_and_binary_sensor_cannot_share_an_address(
    repo_root, tmp_path
):
    def transform(text):
        return text.replace(
            "\nbinary_sensor:\n",
            "\nbinary_sensor:\n"
            "  - platform: visslink\n"
            "    visslink_id: viessmann\n"
            '    name: "Doublon"\n'
            "    address: 0x2303\n",
            1,
        )

    result = _run_config(repo_root, tmp_path, transform)
    out = result.stdout + result.stderr
    assert result.returncode != 0
    assert "declared by several visslink entities" in out, out


def test_python_and_cpp_eco_party_tables_match(repo_root):
    header = (repo_root / "components/visslink/write_guard.h").read_text(
        encoding="utf-8"
    )
    flags = {
        int(a, 16): (int(p, 16), party == "true", m2 == "true")
        for a, p, party, m2 in re.findall(
            r"\{0x([0-9A-Fa-f]{4}), 0x([0-9A-Fa-f]{4}), (true|false), (true|false)\}",
            header,
        )
    }
    assert flags == {
        0x2302: (0x2303, False, False),
        0x2303: (0x2302, True, False),
        0x3302: (0x3303, False, True),
        0x3303: (0x3302, True, True),
    }
    switch_py = (repo_root / "components/visslink/switch.py").read_text(
        encoding="utf-8"
    )
    block = switch_py.partition("ECO_PARTY_FLAGS = {")[2].partition("}")[0]
    py_flags = {
        int(a, 16): (c, f)
        for a, c, f in re.findall(
            r'0x([0-9A-Fa-f]{4}): \("(A1|M2)", "(Eco|Party)"\)', block
        )
    }
    assert py_flags == ECO_PARTY_FLAGS
    for address, (circuit, function) in ECO_PARTY_FLAGS.items():
        assert flags[address][1] == (function == "Party")
        assert flags[address][2] == (circuit == "M2")
    cpp_limits = {
        int(a, 16): (int(lo), int(hi))
        for a, lo, hi in re.findall(
            r"\{0x([0-9A-Fa-f]{4}), (\d+), (\d+), (?:true|false)\},\s*//", header
        )
    }
    number_py = (repo_root / "components/visslink/number.py").read_text(
        encoding="utf-8"
    )
    block = number_py.partition("PARTY_WRITE_LIMITS = {")[2].partition("}")[0]
    py_limits = {
        int(a, 16): (int(lo), int(hi))
        for a, lo, hi in re.findall(r"0x([0-9A-Fa-f]{4}): \((\d+), (\d+)\)", block)
    }
    assert cpp_limits == py_limits == PARTY_LIMITS
    for address, (lo, hi) in PARTY_LIMITS.items():  # dans la plage documentée
        assert PARTY_WRITABLE[address][0] <= lo < hi <= PARTY_WRITABLE[address][1]
    # M2 plancher : jamais au-delà du plafond de sa consigne confort.
    assert PARTY_LIMITS[0x3308][1] == HEATING[0x3306][1]


@pytest.mark.parametrize("address", sorted(PARTY_LIMITS))
def test_party_setpoint_number_bounds(repo_root, tmp_path, address):
    def transform(text):
        return text.replace(
            f"    address: 0x{address:04X}\n    min_value: 16\n",
            f"    address: 0x{address:04X}\n    min_value: 2\n",
            1,
        )

    result = _run_config(repo_root, tmp_path, transform)
    out = result.stdout + result.stderr
    assert result.returncode != 0
    assert "min_value must be at least 3" in out, out


def test_eco_party_v091_cpp_never_writes_comfort_reduced_or_at_boot(repo_root):
    """Les chemins Éco/Party n'utilisent que leurs adresses ; aucun appel
    d'écriture dans setup() ; le climate M2 n'a toujours pas de preset."""
    cpp = (repo_root / "components/visslink/visslink.cpp").read_text(encoding="utf-8")
    setup = cpp.partition("void VissLinkComponent::setup() {")[2].partition("\n}\n")[0]
    assert "submit" not in setup and "request_write" not in setup
    flag_code = cpp.partition("// Commandes Éco / Party A1 et M2 (v0.9.1")[2].partition(
        "void VissLinkComponent::dump_config()"
    )[0]
    assert flag_code
    for address in ("0x2306", "0x2307", "0x3306", "0x3307", "0x6300"):
        assert address not in flag_code
    eco = (repo_root / "components/visslink/eco_party.h").read_text(encoding="utf-8")
    for address in ("0x2306", "0x2307", "0x3306", "0x3307"):
        assert address not in eco


# --- v1.0.0 : configuration définitive ----------------------------------------

V1_NAMES = {
    "Bouclage ECS - Pompe bouclage": 0x0846,
    "Radiateurs - Éco": 0x2302,
    "Radiateurs - Party": 0x2303,
    "Radiateurs - Consigne Party": 0x2308,
    "Plancher chauffant - Éco": 0x3302,
    "Plancher chauffant - Party": 0x3303,
    "Plancher chauffant - Consigne Party": 0x3308,
}
HEATING_CURVES = (0x2304, 0x2305, 0x3304, 0x3305)


def _section(out, name):
    """Section de premier niveau de la sortie de `esphome config`."""
    body = out.partition(f"\n{name}:\n")[2]
    match = re.search(r"\n[a-z_]+:", body)
    return body[: match.start()] if match else body


def _entries(section):
    """Entrées `- ...` d'une section de la sortie de `esphome config`."""
    return [e for e in re.split(r"\n(?=  - )", section) if e.strip()]


def test_v1_example_valid_and_each_address_once(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "Configuration is valid" in out
    addresses = re.findall(r"^    address: (0x[0-9A-Fa-f]+)$", out, re.MULTILINE)
    assert addresses, out
    assert len(addresses) == len(set(addresses)), sorted(addresses)


def test_v1_example_has_no_authorization_option(repo_root):
    """Ni dans les entités actives ni dans les commentaires."""
    text = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    for option in REMOVED_OPTIONS:
        assert option not in text, option
    assert "enable_" not in text
    assert "allow_party_write" not in text
    # Capteurs expérimentaux abandonnés : aucun, même commenté.
    for address in ("A38F", "0x2544", "0x3544", "0x0845", "0x0883", "0x2906"):
        assert address.upper() not in text.upper(), address
    assert not re.search(r"^\s*# - platform", text, re.MULTILINE)


def test_v1_example_references_the_release(repo_root):
    text = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    sources = re.findall(r"- source: (\S+)", text)
    assert sources == [GITHUB_SOURCE], sources
    assert (
        "components: [visslink, visslink_boost, visslink_schedule_editor, "
        "visslink_burner]" in text
    )


@pytest.mark.parametrize(
    "case, transform",
    [
        (
            "enable_writes",
            lambda s: s.replace(
                "  max_retries: 2", "  max_retries: 2\n  enable_writes: true", 1
            ),
        ),
        (
            "enable_schedule_writes",
            lambda s: s.replace(
                "  max_retries: 2",
                "  max_retries: 2\n  enable_schedule_writes: true",
                1,
            ),
        ),
        (
            "validated_days",
            lambda s: s.replace(
                "\n  time_id: ha_time\n",
                "\n  time_id: ha_time\n  validated_days: [monday]\n",
                1,
            ),
        ),
        (
            "allow_write_number",
            lambda s: s.replace(
                "    address: 0x2308\n",
                "    address: 0x2308\n    allow_write: true\n",
                1,
            ),
        ),
        (
            "allow_write_switch",
            lambda s: s.replace(
                "    address: 0x2303\n",
                "    address: 0x2303\n    allow_write: true\n",
                1,
            ),
        ),
        (
            "status_on_number",
            lambda s: s.replace(
                "    address: 0x2306\n",
                '    address: 0x2306\n    status:\n      name: "État"\n',
                1,
            ),
        ),
        (
            "status_on_switch",
            lambda s: s.replace(
                "    address: 0x2302\n",
                '    address: 0x2302\n    status:\n      name: "État"\n',
                1,
            ),
        ),
    ],
)
def test_v1_removed_options_are_rejected(repo_root, tmp_path, case, transform):
    text = (repo_root / EXAMPLE).read_text(encoding="utf-8")
    assert transform(text) != text, case
    result = _run_config(repo_root, tmp_path, transform)
    assert result.returncode != 0, f"{case} should be rejected"


def test_v1_no_diagnostic_entities(repo_root, tmp_path):
    """Les entités VissLink, le réseau et l'uptime sont dans « Capteurs » ;
    seul le bouton « Oublier la sauvegarde » reste en « Configuration »."""
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "entity_category: diagnostic" not in out, out
    assert out.count("entity_category: config") == 1, out
    forget = out.partition("  forget:\n")[2].partition("\n  ")[0]
    assert "Oublier la sauvegarde" in forget
    for name in (
        "VissLink - Signal Wi-Fi",
        "VissLink - Uptime",
        "VissLink - Adresse IP",
        "VissLink - Réseau Wi-Fi",
    ):
        entry = out.partition(f"name: {name}\n")[2][:400]
        assert "entity_category: ''" in entry, name


def test_v1_required_names_and_party_sliders(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    for name, address in V1_NAMES.items():
        match = re.search(
            rf"name: {re.escape(name)}\n(?:    [^\n]*\n)*?    address: 0x{address:X}\n",
            out,
        )
        assert match, name
    numbers = _entries(_section(out, "number"))
    party = {}
    for entry in numbers:
        address = re.search(r"address: (0x[0-9A-Fa-f]+)", entry)
        if address and int(address.group(1), 16) in (0x2308, 0x3308):
            party[int(address.group(1), 16)] = entry
    assert set(party) == {0x2308, 0x3308}
    for address, (lo, hi) in {0x2308: (16, 23), 0x3308: (16, 22)}.items():
        entry = party[address]
        assert "mode: SLIDER" in entry, entry
        assert f"min_value: {lo}\n" in entry and f"max_value: {hi}\n" in entry, entry
    switches = _entries(_section(out, "switch"))
    assert len(switches) == 4, switches
    for entry in switches:
        assert "restore_mode: DISABLED" in entry, entry


def test_v1_example_contents(repo_root, tmp_path):
    result = _run_config(repo_root, tmp_path)
    out = result.stdout + result.stderr
    assert result.returncode == 0, out
    assert "  name: iot-viessmann\n" in out
    uart = _section(out, "uart")
    for line in ("baud_rate: 4800", "parity: EVEN", "stop_bits: 2", "data_bits: 8"):
        assert line in uart, line
    for component in (
        "visslink",
        "visslink_boost",
        "visslink_schedule_editor",
        "visslink_burner",
        "climate",
    ):
        assert f"\n{component}:\n" in out, component
    for action in EDITOR_ACTIONS:
        assert f"action: {action}\n" in out, action
    # Courbes de chauffe : lecture seule (sensor), jamais une commande.
    sensors = _section(out, "sensor")
    for address in HEATING_CURVES:
        assert f"    address: 0x{address:X}\n" in sensors, hex(address)
        for section in ("number", "select", "switch"):
            assert f"address: 0x{address:X}\n" not in _section(out, section)


def test_v1_no_authorization_code_left(repo_root):
    forbidden = (
        "enable_writes",
        "enable_mode_writes",
        "enable_m2_mode_write",
        "enable_schedule_writes",
        "enable_dhw_setpoint_write",
        "enable_heating_setpoint_writes",
        "allow_write",
        "allow_party_write",
        "validated_days",
        "Permission",
        "writes_enabled",
        "schedule_writes_enabled",
    )
    paths = [
        p
        for p in sorted((repo_root / "components").rglob("*"))
        if p.suffix in (".py", ".h", ".cpp") and "__pycache__" not in p.parts
    ] + [
        p
        for p in sorted((repo_root / "lovelace").rglob("*"))
        if p.suffix in (".js", ".html")
    ]
    assert paths
    for path in paths:
        text = path.read_text(encoding="utf-8")
        for word in forbidden:
            assert word not in text, f"{path.relative_to(repo_root)}: {word}"
