# Raccourcis de développement VissLink.
EXAMPLE := examples/iot-viessmann-v1.0.0.yaml
# Exemple compilé avec les composants du dépôt (au lieu de github://...@v1.0.0)
# et des secrets fictifs : build/example.

.PHONY: test cpp-test lint config compile secrets compat compat-v0.1 compat-v0.2 compat-v0.2.2 compat-v0.2.2-diagnostic compat-v0.3 compat-v0.4 compat-v0.4.1

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

build/example/iot-viessmann.yaml: $(EXAMPLE)
	mkdir -p build/example
	sed 's|- source: github://ubikyo/visslink@v1.0.0|- source: {type: local, path: $(CURDIR)/components}|' \
	  $(EXAMPLE) > build/example/iot-viessmann.yaml
	printf 'wifi_ssid: "dummy"\nwifi_password: "dummy-password"\nfallback_ap_password: "dummy-fallback"\napi_encryption_key: "%s"\n' \
	  "$$(openssl rand -base64 32)" > build/example/secrets.yaml

config: build/example/iot-viessmann.yaml
	esphome config build/example/iot-viessmann.yaml

compile: build/example/iot-viessmann.yaml
	esphome compile build/example/iot-viessmann.yaml

# Rétrocompatibilité : les YAML de la v0.1.0, de la v0.2, de la v0.2.2
# (production, identique à la v0.2.1, et diagnostic), de la v0.3, de la v0.4
# et de la v0.4.1, inchangés
# (tests/fixtures), doivent rester valides et compiler. v1.0.0 : seule
# migration, les options d'autorisation `enable_*` (supprimées) sont retirées. Construits dans
# build/compat-vXX avec des secrets fictifs ; examples/secrets.yaml n'est pas
# utilisé.
# v0.2.2 : exemple de production identique à celui de la v0.2.1.
compat: compat-v0.1 compat-v0.2 compat-v0.2.2 compat-v0.2.2-diagnostic compat-v0.3 compat-v0.4 compat-v0.4.1

compat-v0.1 compat-v0.2 compat-v0.2.2 compat-v0.2.2-diagnostic compat-v0.3 compat-v0.4 compat-v0.4.1: compat-%:
	mkdir -p build/compat-$*
	sed -e 's|path: ../components|path: $(CURDIR)/components|' \
	  -e '/^  enable_[a-z0-9_]*: /d' \
	  tests/fixtures/iot-viessmann-$*.yaml > build/compat-$*/iot-viessmann.yaml
	printf 'wifi_ssid: "dummy"\nwifi_password: "dummy-password"\nfallback_ap_password: "dummy-fallback"\napi_encryption_key: "%s"\n' \
	  "$$(openssl rand -base64 32)" > build/compat-$*/secrets.yaml
	esphome config build/compat-$*/iot-viessmann.yaml
	esphome compile build/compat-$*/iot-viessmann.yaml
