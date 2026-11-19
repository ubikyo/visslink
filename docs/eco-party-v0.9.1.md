# Éco et Party (v0.9.1) : pilotage expérimental

> **v1.0.0 :** document historique. Les options d'autorisation (`enable_*`,
> `allow_write`, `validated_days`), les entités « État modification / commande »
> et les exemples intermédiaires cités ici n'existent plus. État actuel :
> `docs/v1.0.0.md` et `examples/iot-viessmann-v1.0.0.yaml`.


Exemple : `examples/iot-viessmann-eco-party-v0.9.1.yaml`. Analyse des sources
(v0.9) : `docs/eco-party-v0.9.md`.

## 1. Point de départ

| Registre | Fonction | Lecture sur la KW3 (209C) | Écriture |
|---|---|---|---|
| `0x2302` | Éco A1 (radiateurs) | validée, 0/1 | documentée pour la 200 KW2 seulement |
| `0x2303` | Party A1 | validée, 0/1 | documentée pour la 200 KW2 seulement |
| `0x2308` | Consigne Party A1 | validée, °C entiers | documentée pour la 200 KW2 seulement |
| `0x3302` | Éco M2 (plancher) | validée, 0/1 | **documentée pour aucun appareil** |
| `0x3303` | Party M2 | validée, 0/1 | documentée pour la 200 KW2 seulement |
| `0x3308` | Consigne Party M2 | validée, °C entiers | documentée pour la 200 KW2 seulement |

Une lecture qui fonctionne ne prouve pas que l'écriture est acceptée. Les
commandes sont donc livrées **désactivables une par une** et se valident
sur la chaudière dans l'ordre du § 3.

## 2. Ce qui est implémenté

### Entités Home Assistant

| Entité | Plateforme | Registre | Lecture |
|---|---|---|---|
| Radiateurs - Party / Éco | `switch` | `0x2303` / `0x2302` | 60 s |
| Plancher chauffant - Party / Éco | `switch` | `0x3303` / `0x3302` | 60 s |
| Radiateurs - Consigne Party | `number` | `0x2308` | 5 min |
| Plancher - Consigne Party | `number` | `0x3308` | 5 min |

Chaque entité a un texte d'état facultatif (`status`). Les quatre
`binary_sensor` et les deux `sensor` de la v0.9 sont remplacés : dans Home
Assistant, ce sont de nouvelles entités (`switch.*`, `number.*`), à
reprendre dans les tableaux de bord et les automatisations.

### Autorisations

Une écriture n'est émise que si :

- `enable_writes` est vrai (défaut, cf. `docs/ecriture-kw.md`) ;
- l'option `allow_write` de l'entité est vraie (défaut). `allow_write: false`
  laisse l'entité en lecture seule. L'option n'existe que sur ces six
  entités ;
- l'adresse est dans la liste blanche propre à la fonction :
  `ECO_PARTY_FLAGS` (4 commandes, valeurs `00`/`01` seulement) ou
  `PARTY_SETPOINTS` (2 consignes).

Ces listes sont distinctes de toutes les autres. Aucune autre autorisation
(consignes, programmes, blocs horaires) ne les ouvre, et `allow_write` n'ouvre
rien d'autre. Tout est revérifié juste avant l'émission.

### Transaction

Même chemin que les consignes ECS et chauffage (v0.6/v0.7) :

1. **Demande** depuis Home Assistant : autorisations, liaison, valeur.
   Refus si une autre commande Éco/Party est en cours, moins de 15 s après
   une autre commande Éco/Party, ou moins de 30 s après une commande du même
   registre. Les consignes Party partagent la garde des autres consignes.
2. **Relecture préalable** du registre. Valeur déjà égale : rien n'est
   écrit (« Inchangé »). Valeur lue autre que `00`/`01` : abandon, rien
   n'est écrit.
3. **Écriture d'un octet**, via l'arbitre commun : une écriture à la fois,
   10 s au moins entre deux, demande abandonnée après 120 s sans émission,
   attente derrière la restauration du bouclage ECS.
4. **Relecture de vérification**. Pour Éco/Party, l'autre commande du même
   circuit est relue juste après (Party après Éco, Éco après Party).
5. **Résultat** dans le texte d'état : « Confirmé », « Échec : relu … au lieu
   de … ; pas de nouvel essai » ou « Indéterminé ».

Jamais de nouvel essai automatique, jamais de commande au démarrage :
`restore_mode` est imposé à `DISABLED`, `inverted` est refusé. Rien n'est
gardé après un redémarrage de l'ESP.

### Limites des consignes Party

| Registre | Plage documentée (KW2) | Limites internes (provisoires) |
|---|---|---|
| `0x2308` A1 | 3..37 °C | 16..23 °C |
| `0x3308` M2 | 3..37 °C | 16..22 °C |

- **Choix des limites :** ce sont celles de la consigne confort du même
  circuit. Le plancher reste plafonné à 22 °C à cause de son inertie.
- **Bornes du YAML :** `min_value` et `max_value` restreignent encore la
  plage, sans jamais l'élargir.
- **Usage « 4 °C = ECS seule » (notice p. 31) :** il n'est pas possible
  depuis VissLink.

### Ce qui n'est jamais fait

- **Consignes confort et réduite** (`0x2306`, `0x2307`, `0x3306`, `0x3307`) :
  jamais modifiées pour simuler Éco ou Party.
- **Climate M2 :** inchangé (HEAT/OFF, Chauffe/Inertie/Arrêt). Aucun preset
  Éco/Party, tant que l'écriture M2 n'est pas validée (§ 3, étapes 4 à 6).
- **Exclusion entre Éco et Party :** aucune n'est supposée. Si la régulation
  coupe l'un quand l'autre est activé, la relecture le montre :
  « Modifié par la régulation après la commande Party : activé ->
  désactivé ».

### Synchronisation avec la Vitotronic et la Vitotrol

- **Lecture périodique :** l'état affiché est toujours l'état lu, toutes les
  60 s pour Éco/Party et toutes les 5 min pour les consignes.
- **Changement hors de Home Assistant :** une commande au panneau ou à la
  Vitotrol, ou une fin automatique, est signalée ainsi :
  - Éco/Party : « Modifié hors de Home Assistant (fin automatique, panneau ou
    Vitotrol ?) » ;
  - consignes : « Modifiée hors de Home Assistant (panneau ou Vitotrol ?) ».
- **Fins automatiques (notice KW3) :**
  - Éco se termine au prochain passage en température réduite ;
  - Party se termine au prochain passage en température normale.
- **Ce que VissLink ne fait jamais :** réactiver une fonction terminée par
  la régulation, ou réécrire une valeur modifiée au panneau.
- **Valeur lue autre que `00`/`01` :** elle n'est pas interprétée. L'état
  affiché reste le dernier état valide, et le texte d'état signale
  « Valeur inattendue ».

## 3. Validation sur la chaudière, registre par registre

L'exemple est livré à l'**étape 1** : seule la consigne Party A1 a
`allow_write: true`. À chaque étape réussie, passer `allow_write: true` sur
la ligne suivante et reflasher. Ne jamais ouvrir deux étapes à la fois.

| Étape | Entité | Essai |
|---|---|---|
| 1 | Consigne Party A1 `0x2308` | valeur lue + 1 °C, puis retour à la valeur d'origine |
| 2 | Party A1 `0x2303` | activer, puis désactiver |
| 3 | Éco A1 `0x2302` | activer, puis désactiver |
| 4 | Consigne Party M2 `0x3308` | valeur lue ± 1 °C, puis retour |
| 5 | Party M2 `0x3303` | activer, puis désactiver |
| 6 | Éco M2 `0x3302` | en dernier : écriture documentée nulle part |

**M2 et la Vitotrol 200 :** si le panneau affiche « Fernbedienung » pour
M2, les réglages de ce circuit se font à la commande à distance (notice
p. 43). Une écriture Optolink peut alors être accusée sans effet. VissLink
l'affichera en « Échec : relu … au lieu de … » et ne réessaiera pas. Dans
ce cas, laisser les étapes 4 à 6 à `allow_write: false`.

**Avant chaque essai :**

- noter les valeurs lues de `0x2301`, `0x3301`, `0x2306`, `0x2307`,
  `0x3306`, `0x3307`, `0x6300` et de l'autre commande du circuit ;
- faire l'essai de jour, panneau (ou Vitotrol) sous les yeux ;
- garder le journal ESPHome ouvert.

**Un essai est réussi si :**

- le journal montre l'accusé (`write acknowledged`) et
  `write confirmed by read-back` ;
- le texte d'état affiche « Confirmé » ou « Confirmée » ;
- le panneau (ou la Vitotrol) montre la fonction ou la consigne, par exemple
  le voyant de la touche Party ;
- le circuit réagit : consigne de départ, circulateur. Pour Party, l'ECS et
  la pompe de bouclage aussi ;
- la commande inverse (désactiver, ou revenir à la valeur d'origine)
  fonctionne de la même façon ;
- la fin automatique a lieu à la bascule de plage suivante (étapes 2, 3, 5,
  6) ;
- aucun des registres notés n'a changé, sauf l'autre commande du circuit si
  la régulation l'a modifiée. Dans ce cas, le noter : c'est la réponse à la
  question « Éco + Party ».

**Un essai échoue si :**

- l'accusé est négatif ;
- le texte d'état affiche « Échec » ou « Indéterminé » ;
- le panneau ne montre rien.

En cas d'échec, remettre `allow_write: false` sur cette entité, noter le
journal et ne pas passer à l'étape suivante.

## 4. Tests

**C++ (PC, ASan/UBSan) :**

- `test_eco_party.cpp` : chemin de `FlagWriter` (refus, relecture préalable,
  confirmation, échec, indéterminé, valeur inattendue, délais, garde commune,
  synchronisation avec le panneau et l'autre commande), consignes Party dans
  `SetpointWriter` (autorisations, limites, indépendance des consignes
  confort/réduite) ;
- `test_write_guard.cpp` : listes blanches exactes et séparées, autorisations.

**Simulation (`test_simulation.cpp`, `sim_v091_*`) :**

- étape 1 seule : une seule trame, vers `0x2308` ;
- les six fonctions dans l'ordre du § 3, trames exactes ;
- aucune écriture sans demande, sans autorisation ou après un redémarrage ;
- écriture ignorée (hypothèse Vitotrol) ou refusée : échec affiché, aucun
  nouvel essai ;
- Éco coupé par Party (hypothèse de test) : affiché, jamais réécrit ;
- changements au panneau ou à la Vitotrol, fin automatique, valeur
  inattendue ;
- 2 h d'usage : lectures périodiques conservées, consignes confort/réduite
  jamais écrites.

Ces simulations reposent sur un modèle de la chaudière. **Elles ne valident
pas le matériel** : seule la procédure du § 3 le fait.

**Python (`test_esphome_config.py`) :**

- exemple valide (étape 1) ;
- seules les entités Éco/Party changent par rapport à la v0.9 ;
- adresses refusées ;
- `restore_mode` et `inverted` contrôlés ;
- doublon `switch`/`binary_sensor` refusé ;
- `allow_write` seulement sur les consignes Party ;
- tables Python et C++ concordantes.

**Compilation pour D1 mini de l'exemple :** RAM 57,7 %, flash 53,1 %.
