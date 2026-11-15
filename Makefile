# Raccourcis de développement VissLink.
EXAMPLE := examples/iot-viessmann.yaml

.PHONY: test cpp-test lint config compile secrets compat compat-v0.1 compat-v0.2 compat-v0.2.2 compat-v0.2.2-diagnostic compat-v0.3 compat-v0.4

# Tests C++ + validation de configuration ESPHome
test:
	pytest -v

# Tests C++ seuls (avec AddressSanitizer / UBSan)
cpp-test:
	cmake -S tests/cpp -B tests/cpp/build -G Ninja
	cmake --build tests/cpp/build
	./tests/cpp/build/visslink_tests

lint:
	ruff check components tests
	ruff format --check components tests

# Crée un examples/secrets.yaml FICTIF (ignoré par git) s'il n'existe pas.
secrets:
	@test -f examples/secrets.yaml || { \
	  printf 'wifi_ssid: "dummy"\nwifi_password: "dummy-password"\nfallback_ap_password: "dummy-fallback"\napi_encryption_key: "%s"\n' \
	    "$$(openssl rand -base64 32)" > examples/secrets.yaml; \
	  echo "examples/secrets.yaml fictif créé"; }

config: secrets
	esphome config $(EXAMPLE)

compile: secrets
	esphome compile $(EXAMPLE)

# Rétrocompatibilité : les YAML de la v0.1.0, de la v0.2, de la v0.2.2
# (production, identique à la v0.2.1, et diagnostic), de la v0.3 et de la v0.4,
# inchangés
# (tests/fixtures), doivent rester valides et compiler. Construits dans
# build/compat-vXX avec des secrets fictifs ; examples/secrets.yaml n'est pas
# utilisé.
# v0.2.2 : exemple de production identique à celui de la v0.2.1.
compat: compat-v0.1 compat-v0.2 compat-v0.2.2 compat-v0.2.2-diagnostic compat-v0.3 compat-v0.4

compat-v0.1 compat-v0.2 compat-v0.2.2 compat-v0.2.2-diagnostic compat-v0.3 compat-v0.4: compat-%:
	mkdir -p build/compat-$*
	sed 's|path: ../components|path: $(CURDIR)/components|' \
	  tests/fixtures/iot-viessmann-$*.yaml > build/compat-$*/iot-viessmann.yaml
	printf 'wifi_ssid: "dummy"\nwifi_password: "dummy-password"\nfallback_ap_password: "dummy-fallback"\napi_encryption_key: "%s"\n' \
	  "$$(openssl rand -base64 32)" > build/compat-$*/secrets.yaml
	esphome config build/compat-$*/iot-viessmann.yaml
	esphome compile build/compat-$*/iot-viessmann.yaml
