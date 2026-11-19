# Éco et Party (v0.9) : analyse des registres et procédure d'essai

Statut : **lecture seule**. Aucune commande Éco ou Party n'est implémentée.
L'analyse ci-dessous montre qu'aucune source ne documente ces écritures pour
la Vitotronic 300 KW3 (`209C`). Elle précise ce qu'il faut vérifier sur la
chaudière avant de les implémenter.

> v0.9.1 : la lecture des six registres est validée sur la KW3 ; les
> commandes sont livrées, désactivables une par une : `docs/eco-party-v0.9.1.md`.

Exemple : `examples/iot-viessmann-eco-party-v0.9.yaml`. C'est l'exemple
v0.8.1 plus la lecture des consignes Party A1 et M2.

## 1. Sources consultées

| Source | Version | Contenu utile | Appareil couvert |
|---|---|---|---|
| vcontrold `xml/kw/vito.xml` | `master`, 2026-11 | `getBetriebSparM1/M2` (`0x2302`, `0x3302`), `getBetriebPartyM1/M2` (`0x2303`, `0x3303`), `getTempPartyM1/M2` (`0x2308`, `0x3308`) ; `setBetriebSparM1` (`0x2302`), `setBetriebPartyM1/M2` (`0x2303`, `0x3303`), `setTempPartyM1/M2` (`0x2308`, `0x3308`). **Aucun `setBetriebSparM2`** | Vitotronic 200 KW2 (`2098`) |
| vcontrold `xml/300/vito.xml` | `master`, 2026-11 | mêmes adresses et mêmes commandes, et toujours aucun `setBetriebSparM2` | mélange KW2 / P300 |
| vcontrold `xml/kw/vcontrold.xml` | `master`, 2026-11 | unité `RT` : `00` → 0, `01` → 1, autre → « NOT OK » ; unité `ST` : 1 octet, valeur directe | — |
| Wiki openv, « Adressen » | 2026-11 | Sparbetrieb `0x2302` (0..1), Partybetrieb `0x2303` (0..1), Party Temperatur Soll `0x2308` (3..37 °C) ; aucune adresse M2 ; pour la V200KO1B, `0x2301` est signalé « cannot write » | V200KW2/KW1, entre autres. **Pas de colonne KW3** |
| Wiki openv, « Geräte » | 2026-11 | V300KW3 = `209C`, protocole KW, 2 circuits avec mélangeur | — |
| Notice Viessmann 5581 479, 2005, « Vitotronic 300, KW3 / Vitotronic 200, KW2 » | — | fonctionnement d'Éco et de Party (p. 23, 24, 31, 32), commande à distance (p. 7, 43) | KW3 et KW2 |
| Forum ioBroker, adaptateur Viessmann | — | un utilisateur dit que l'écriture de `0x2303` n'a pas marché tout de suite, un autre qu'elle a marché | appareils non précisés |

Le dépôt montre déjà qu'une définition KW2 ne vaut pas pour la KW3. vcontrold
code `0x2301` ainsi : `00` ECS, `01` réduit, `02` normal, `04` chauffage +
ECS. Sur cette KW3, les valeurs validées sont `00` (ECS seule), `03`
(chauffage + ECS) et `05` (arrêt). Une adresse identique ne garantit donc ni
le même codage ni la même possibilité d'écriture.

## 2. Réponses aux cinq questions

### 2.1 Écriture réelle de `0x2302`, `0x2303`, `0x3302`, `0x3303`

| Registre | Lecture documentée | Écriture documentée | Pour la KW3 |
|---|---|---|---|
| `0x2302` Éco A1 | KW2 | KW2 (`setBetriebSparM1`) | aucune source |
| `0x2303` Party A1 | KW2 | KW2 (`setBetriebPartyM1`) ; résultats contradictoires sur un forum | aucune source |
| `0x3302` Éco M2 | KW2 | **aucune**, sur aucun appareil | aucune source |
| `0x3303` Party M2 | KW2 | KW2 (`setBetriebPartyM2`) | aucune source |

Ces quatre registres restent en lecture seule. Leur lecture n'est pas non
plus confirmée sur cette KW3 : la procédure de `docs/registres.md` § 6 n'a
pas encore de résultat consigné.

### 2.2 Consignes Party A1 et M2

- **Adresses :** `0x2308` (A1) et `0x3308` (M2), 1 octet, degrés entiers.
- **Plage :** 3..37 °C selon le wiki, pour la KW2.
- **Sur cette KW3 :** l'adresse, le codage et la plage sont à confirmer
  (§ 4).
- **v0.9 :** lecture seule, toutes les 5 min, sous le nom « Consigne Party
  (lecture) ».

### 2.3 Restrictions de commande liées à la Vitotrol

La notice KW3 (p. 43) indique : si le panneau affiche « Fernbedienung »,
c'est qu'une commande à distance est raccordée au circuit, et ses réglages
et consultations doivent se faire sur cette commande.

- **M2 :** M2 a une Vitotrol 200. Si le panneau affiche « Fernbedienung »
  pour M2, la Vitotronic réserve Éco et Party de M2 à la Vitotrol.
- **Inconnu :** aucune source ne dit si une écriture Optolink sur `0x3302` ou
  `0x3303` serait alors acceptée, ignorée ou écrasée par la Vitotrol. On ne
  sait pas non plus si la Vitotrol afficherait le changement.
- **A1 :** A1 n'a pas de commande à distance. Le panneau est la référence.

### 2.4 Arrêt automatique des dérogations

D'après la notice KW3, p. 23, 24 et 31 :

- **Éco :** il abaisse la température normale et se termine seul **au
  prochain passage en température réduite** prévu par la programmation. On
  peut aussi le désactiver avant.
- **Party :** le circuit chauffe à la **température Party**, l'ECS est
  réchauffée jusqu'à sa consigne et **la pompe de bouclage tourne**. Party se
  termine seul **au prochain passage en température normale**. On peut aussi
  le désactiver avant.
- **Durée maximale de Party :** cette notice n'en donne pas. Certaines
  notices d'autres Vitotronic mentionnent « au plus 8 h » ; ce n'est pas
  établi pour la KW3.
- **Party pour l'ECS seule** (p. 31) : régler la température Party à 4 °C.
- **Charge ECS unique** (p. 32) : elle n'est possible ni en « Arrêt », ni
  pendant le programme vacances.

Conséquence : VissLink n'a pas à programmer la fin des dérogations, la
régulation s'en charge. En revanche, l'état affiché doit toujours être
l'état **relu**, puisqu'une dérogation peut s'arrêter sans aucune commande.

### 2.5 Éco et Party demandés ensemble

- **Notice :** elle ne décrit pas ce cas. Éco s'applique pendant une période
  normale et Party pendant une période réduite. Les deux ont chacun leur
  touche, et le voyant s'éteint à la fin.
- **Non documenté :** activer l'un annule-t-il l'autre ? Les deux octets
  peuvent-ils valoir `01` en même temps ?
- **Décision :** pas de logique d'exclusion inventée. VissLink affichera ce
  qu'il relit (§ 4, étape A5).

## 3. Ce qui est implémenté en v0.9

| Fonction | État |
|---|---|
| États Éco et Party A1/M2 (`binary_sensor`, 60 s) | déjà présents (v0.5), inchangés |
| Lecture des consignes Party A1/M2 (`0x2308`, `0x3308`) | **ajoutée** à l'exemple v0.9, en `sensor` uint8 toutes les 5 min, sans code nouveau |
| Changements faits à la Vitotronic ou à la Vitotrol | visibles à la lecture suivante (60 s ou 5 min) |
| Commandes Éco et Party, modification de la consigne Party | **non implémentées** (§ 2.1, § 2.3) |
| Presets du climate M2 | **non ajoutés** : HEAT/OFF et états Chauffe/Inertie/Arrêt inchangés |
| Consignes confort et réduite | inchangées, jamais utilisées pour imiter Éco ou Party |

Verrous ajoutés :

- les six adresses ne figurent dans aucune liste blanche : consignes,
  programmes, bloc du bouclage, éditeur des programmations ;
- une déclaration en `number` ou en `select` est refusée à la validation.

Tests :

- C++ :
  - `v09_eco_party_and_party_setpoints_stay_read_only` ;
  - simulation `sim_v09_party_setpoints_read_only_with_eco_party` : 2 h de
    lectures, écritures autorisées par ailleurs, aucune trame d'écriture.
    Un changement fait à la Vitotrol est relu.
- Python :
  - `test_eco_party_v09_example_reads_without_commands` ;
  - `test_eco_party_v09_example_only_adds_party_setpoint_reads` ;
  - `test_party_setpoints_never_writable` ;
  - `test_climate_has_no_eco_party_preset` ;
  - `test_eco_party_registers_absent_from_cpp_whitelists`.

Mesure de compilation de l'exemple pour D1 mini : RAM 56,2 %, flash 52,0 %.

## 4. À vérifier sur la chaudière avant toute écriture

Flasher l'exemple v0.9 ; il n'écrit rien sur ces registres. Lancer
`esphome logs … | tee eco-party.log`. Pour chaque étape, noter l'heure, ce
qu'affichent le panneau et la Vitotrol, et les lignes `0x2302`, `0x2303`,
`0x3302`, `0x3303`, `0x2308` et `0x3308 <- xx` du journal.

### Phase A : lecture (aucune écriture)

| Étape | Action | Résultat attendu pour confirmer |
|---|---|---|
| A0 | Ajouter temporairement un `sensor` `0x00F8`, `uint16` | `20 9C` (KW3). Sinon, s'arrêter là |
| A1 | Procédure de `docs/registres.md` § 6, étapes 1 à 6 (Éco/Party M2 depuis la Vitotrol, A1 depuis le panneau) | `00`/`01` qui suivent chaque commande, sur le bon circuit uniquement |
| A2 | Panneau : choisir A1, appuyer sur Party ; lire la température Party affichée, puis annuler | `0x2308` = la valeur affichée |
| A3 | Changer la température Party de A1 de 1 °C au panneau, attendre 5 min, puis remettre la valeur d'origine | `0x2308` suit les deux changements |
| A4 | Même chose pour M2, là où sa température Party s'affiche (Vitotrol 200, ou panneau) | `0x3308` suit. Si M2 ne se règle qu'à la Vitotrol, le noter |
| A5 | Sur A1 : Party puis Éco, puis Éco puis Party | Relever les deux octets : l'un annule-t-il l'autre ? |
| A6 | Party A1 pendant une période réduite, sans y toucher ensuite | `0x2303` repasse à `00` au passage en température normale. Noter l'heure, et toute fin anticipée (8 h ?) |
| A7 | Éco A1 pendant une période normale, sans y toucher ensuite | `0x2302` repasse à `00` au passage en température réduite |
| A8 | Panneau : choisir M2 | Le panneau affiche-t-il « Fernbedienung » ? Si oui, M2 est réservé à la Vitotrol |
| A9 | Party A1 avec le circuit en « ECS seule », puis en « Arrêt » | Party est-il accepté ? Noter `0x2303` et le voyant |

La lecture est confirmée seulement si A1 à A4 se vérifient. Sinon, ces
registres ne correspondent pas à cet usage sur la KW3. Dans ce cas, les
entités sont à retirer, ou à garder en `uint8` brut comme diagnostic.

### Phase B : premiers essais d'écriture (version ultérieure, sur autorisation)

Prérequis : phase A entièrement confirmée. Une écriture à la fois, un seul
circuit, de jour, avec le panneau sous les yeux. Les valeurs d'origine sont
notées avant chaque essai.

Ordre proposé, du moins risqué au plus risqué :

1. **Consigne Party A1 (`0x2308`)** : ce n'est qu'une valeur, utilisée
   seulement pendant Party. Écrire la valeur lue + 1 °C, dans la plage
   3..37, puis remettre la valeur d'origine.
2. **Party A1 (`0x2303`)** : écrire `01`, puis `00`.
3. **Éco A1 (`0x2302`)** : écrire `01`, puis `00`.
4. **M2 :** seulement si A8 montre que M2 n'est pas réservé à la Vitotrol,
   ou après un essai qui montre l'effet sur la Vitotrol. D'abord `0x3308`,
   puis `0x3303`. **`0x3302` en dernier** : aucune source n'en documente
   l'écriture.

Chaque essai est réussi si toutes ces conditions sont réunies :

- la chaudière accuse réception avec `00` ;
- la relecture donne exactement la valeur écrite ;
- le panneau (ou la Vitotrol) affiche le mode : voyant de la touche allumé ;
- le circuit réagit : consigne de départ, circulateur ;
- l'arrêt par commande (`00`) et la fin automatique (A6, A7) fonctionnent
  toujours ;
- aucun autre registre ne change : `0x2301`, `0x3301`, `0x2306`, `0x2307`,
  `0x3306`, `0x3307`, les programmations et `0x6300` ;
- la valeur est la même après une coupure de courant de la chaudière.

Un essai échoue si l'accusé est négatif, si la relecture diffère, ou si le
panneau ne montre rien. Dans ce cas, le registre reste en lecture seule.

### Code à écrire après confirmation (non fait)

- **Écriture :** ajouter les adresses confirmées aux listes blanches. Éco et
  Party n'acceptent que `00`/`01`. La consigne Party est bornée à 3..37 °C et
  aux bornes du YAML.
- **Garanties :** réutiliser les protections existantes : relecture avant
  écriture, relecture exacte après écriture, pas de nouvel essai
  automatique, écritures espacées, état toujours relu, et mise en attente
  derrière le bouclage ECS comme pour les autres consignes.
- **Home Assistant :** un `switch` par mode et par circuit, dont l'état est
  toujours la valeur relue. Un `number` pour la consigne Party.
- **Climate M2 :** des presets seulement si M2 est commandable (A8, B4). Le
  choix des noms de presets Home Assistant reste à faire.
- **Pompe de bouclage :** Party fait tourner la pompe (§ 2.4). L'état réel
  `0x0846` le montrera. Le bouclage temporaire n'a pas à en tenir compte :
  il ne modifie que la programmation.
