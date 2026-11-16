# Consignes de chauffage A1/M2 et climate du plancher (v0.7, expérimental)

> **Statut : NON validé sur la chaudière.** Les quatre consignes sont lues
> depuis la v0.3. Aucune n'a été écrite sur la Vitotronic 300 KW3. Leur
> écriture est désactivée par défaut et exige deux autorisations. Pour M2
> (`0x3306`, `0x3307`), l'écriture n'est documentée **par aucune source**,
> seulement supposée par analogie avec A1.

## 1. Analyse de l'existant

| Élément | Avant la v0.7 | Risque |
|---|---|---|
| `0x2306`, `0x2307`, `0x3306`, `0x3307` | `number` depuis la v0.3, chemin d'écriture générique | `enable_writes: true` suffisait pour les écrire |
| Contrôle « inchangée » | comparé à la dernière lecture (≤ 5 min) | demande ignorée à tort après un changement au panneau ou à la Vitotrol |
| Limites | plage documentée 3..37 °C, identique pour les deux circuits | aucune limite propre au plancher |
| Demandes rapprochées | la dernière remplaçait la demande en attente | rafale possible depuis une automatisation |
| Programmes `0x2301`/`0x3301` | `select`, validés en écriture sur la KW3 | inchangés |

Sources de l'écriture :

- `0x2306`/`0x2307` : vcontrold `setTempRaumNorSollM1`/`setTempRaumRedSollM1`,
  Vitotronic 200 **KW2**.
- `0x3306`/`0x3307` : aucune commande `set` connue. Seule la table openv
  (« M2 : 0x33.. ») les suggère, par analogie.

## 2. Ce qui change (implémenté)

### Une transaction par consigne

Les quatre consignes utilisent la transaction de la consigne ECS (v0.6), un
`SetpointWriter` par registre :

1. autorisations : `enable_writes` **et** `enable_heating_setpoint_writes`
   (vérifiées à la demande **et** juste avant l'émission) ;
2. valeur entière, dans les bornes du `number` **et** dans les limites
   internes du registre ;
3. confort ≥ réduite du même circuit (dernière valeur lue de l'autre) ;
4. liaison synchronisée, aucune autre modification de consigne en cours ;
5. **relecture** du registre ; identique → rien n'est écrit ;
6. écriture d'un octet, accusé, **relecture immédiate**, comparaison exacte ;
7. publication de la valeur relue ; échec ou issue indéterminée : la
   dernière valeur confirmée reste affichée, **aucune nouvelle tentative**.

Le chemin générique de la v0.3 n'est plus utilisé. Une consigne ne peut
plus être écrite autrement que par sa transaction.

### Autorisation propre

```yaml
visslink:
  enable_writes: false
  enable_heating_setpoint_writes: false   # v0.7, exige enable_writes
```

| Autorisations | Consignes A1/M2 | Consigne ECS | Programmes A1/M2 |
|---|---|---|---|
| `enable_writes` seul | **refusées** (nouveau) | refusée | refusés |
| + `enable_heating_setpoint_writes` | écrites | refusée | refusés |
| + `enable_dhw_setpoint_write` / modes / programmations | **refusées** | selon la sienne | selon les leurs |

### Limites internes provisoires (par registre)

| Registre | Circuit | Limites internes | Pourquoi |
|---|---|---|---|
| `0x2306` | A1 radiateurs, confort | 16..23 °C | plage habituelle d'ambiance |
| `0x2307` | A1 radiateurs, réduite | 10..20 °C | radiateurs réactifs : abaissement profond possible |
| `0x3306` | M2 plancher, confort | 16..**22** °C | la consigne d'ambiance déplace la courbe de chauffe, donc le départ |
| `0x3307` | M2 plancher, réduite | **14**..20 °C | forte inertie : un abaissement profond se rattrape mal |

Elles sont dans la plage documentée (3..37) et **ne remplacent pas** les
protections de la Vitotronic : courbe de chauffe, départ maximal de M2,
vanne mélangeuse, thermostat de sécurité du plancher. Elles sont à revoir
après validation, dans `write_guard.h` **et** `number.py`. Un test vérifie
que les deux concordent.

Les bornes `min_value`/`max_value` existantes (16..24, 10..20) restent
valides pour l'affichage. Elles ne peuvent que **restreindre** l'écriture.

### Une modification de consigne à la fois

Garde commune aux cinq consignes (ECS comprise) :

- une seule modification en cours ;
- 15 s entre deux écritures de consignes **différentes** ;
- 30 s entre deux écritures de la **même** consigne (v0.6).

Sans cette garde, une rafale d'une demande toutes les 2 s faisait tomber
les lectures du brûleur à 340 sur 780 en 6 h 30 (resynchro 4 s), car chaque
écriture coûte trois transactions (relecture, écriture, vérification).
Avec la garde, les bornes de la v0.4.1 sont respectées. Avec la seule
consigne ECS déclarée, rien ne change par rapport à la v0.6.

### Priorité du bouclage ECS

Pendant une restauration ou une récupération du bouclage, **toutes** les
écritures de consignes attendent dans l'arbitre (`OwnerSet`). Les commandes
de programme A1/M2 ne sont pas concernées. Contrôle en simulation : aucune
écriture de consigne démarrée pendant une restauration sur 20 scénarios,
alors que le défaut est détecté sans la priorité.

### Diagnostic

Option `status:` sur chaque `number` de consigne : texte d'état
(« Relecture de la consigne actuelle… », « Confirmée : 20 °C relue
(précédente 21 °C, accusé reçu) », « Modifiée hors de Home Assistant
(panneau ou Vitotrol ?) : 20 -> 19 °C », motif d'un refus).

Journaux :

- demande et valeur précédente ;
- autorisations ;
- valeur relue avant l'écriture ;
- trame `0x3306 -> 14 (write)` ;
- accusé ;
- relecture ;
- `setpoint 20 confirmed 7300 ms after the request` ;
- motif précis de tout refus.

## 3. Climate du plancher M2 (implémenté, essai seulement)

```yaml
climate:
  - platform: visslink
    visslink_id: viessmann
    name: "Plancher chauffant"
    comfort_setpoint_id: m2_comfort_setpoint     # number 0x3306 (obligatoire)
    mode_id: m2_mode                             # select 0x3301 (obligatoire)
    current_temperature_id: m2_room_temperature  # ambiance Vitotrol 0x0898 (facultatif)
    pump_id: m2_pump                             # circulateur 0x3906 (facultatif)
    burner_id: bruleur_actif                     # brûleur 0x0842 (facultatif, v0.7.3)
    status:                                      # text_sensor d'état (facultatif, v0.7.3)
      name: "Plancher chauffant - État"
```

Le climate **ne lit aucun registre et n'écrit rien lui-même**. Il agrège
des entités existantes ; chaque registre n'est lu qu'une fois.

| Élément du climate | Source | Remarque |
|---|---|---|
| Température actuelle | ambiance Vitotrol `0x0898` | observée cohérente, **pas encore validée** (§ 6). Jamais la température de départ : la validation refuse `0x3900`, `0x0802`, `0x0804`, `0x0800` |
| Température cible | consigne **confort** `0x3306` lue | voir ci-dessous |
| Mode | programme `0x3301` lu | `03` → HEAT ; `00` (ECS seule) et `05` (arrêt) → OFF ; autre valeur : mode inchangé, jamais deviné |
| Action et état (v0.7.3) | programme `0x3301`, circulateur `0x3906`, brûleur `0x0842` | voir « État du plancher » ci-dessous. Affichage seulement |
| Température de départ `0x3900` | entité `sensor` séparée | un climate ESPHome n'a pas d'attribut libre |

### État du plancher (v0.7.3)

Calculé à partir des entités déjà lues (programme `0x3301`, circulateur
`0x3906`, brûleur `0x0842`), **sans aucune lecture Optolink
supplémentaire** :

| État (`status`) | Condition | `hvac_action` |
|---|---|---|
| **Chauffe** | chauffage (`03`) + circulateur en marche + brûleur en marche | `heating` |
| **Inertie** | chauffage (`03`) + circulateur en marche + brûleur arrêté | `idle` |
| **Arrêt** | chauffage désactivé (`00` ou `05`) | `off` |
| **Arrêt** | chauffage (`03`) + circulateur arrêté | `idle` |
| **Inconnu** | donnée nécessaire inconnue ou périmée | `idle` (`off` si le mode affiché est OFF) |

- `heating` n'est publié **qu'en Chauffe** : jamais sur le seul circulateur.
  Sans `burner_id`, l'action n'est donc jamais `heating` (Inconnu quand le
  circulateur tourne).
- Seules les données nécessaires comptent : chauffage désactivé → Arrêt quels
  que soient le circulateur et le brûleur ; circulateur arrêté → Arrêt quel
  que soit le brûleur.
- **Données périmées** : une valeur est périmée sans lecture réussie depuis
  **3 intervalles de lecture + 30 s** de son entité (brûleur à 30 s : 2 min ;
  circulateur à 60 s : 3 min 30 s ; programme à 5 min : 15 min 30 s). Elle
  compte alors comme inconnue ; l'état passe à Inconnu. Le mode affiché
  garde la dernière valeur lue (jamais deviné).
- `pump_id` et `burner_id` doivent désigner les `binary_sensor` visslink de
  `0x3906` et `0x0842` (vérifié à la validation).
- Le circulateur `0x3906` reste **à valider** (§ 6) ; l'état n'est qu'un
  affichage, jamais une commande.

**Température cible = consigne confort de M2, en permanence.** Pendant une
période réduite de la programmation, la Vitotronic régule sur la consigne
réduite `0x3307`. Le climate continue d'afficher la consigne confort, car il
ne sait pas de façon sûre quelle période est active (Éco/Party non
confirmés). La consigne réduite reste l'entité « Plancher - Consigne
réduite ».

Commandes :

- **`set_temperature`** passe par la transaction de `0x3306`
  (`enable_heating_setpoint_writes`) :
  - ignorée si elle est égale à la valeur lue ;
  - non répétée si la même valeur est déjà en cours.
- **`set_hvac_mode`** passe par le sélecteur de M2 (`enable_mode_writes` et
  `enable_m2_mode_write`, inchangés) :
  - HEAT → `03` ;
  - OFF → `00` (ECS seule : l'eau chaude reste produite) ;
  - **jamais `05`** ;
  - rien n'est envoyé si le programme lu correspond déjà.
- **Presets** : non implémentés. Éco et Party (`0x3302`/`0x3303`) ne sont
  pas validés sur cette KW3.
- **État publié** : toujours lu, jamais la demande. Rien n'est commandé au
  démarrage de l'ESP. Le climate n'est publié qu'une fois la consigne et le
  programme connus.

## 4. Mesures

Simulation de la demande à la confirmation (relecture préalable, écriture,
vérification) :

| Resynchro | `0x2306` | `0x2307` | `0x3306` | `0x3307` |
|---|---|---|---|---|
| 2 s | 5,3 s | 5,4 s | 5,2 s | 7,2 s |
| 3 s | 6,3 s | 9,4 s | 7,3 s | 9,3 s |
| 4 s | 9,3 s | 13,3 s | 13,3 s | 13,3 s |

Compilation D1 mini (ESPHome 2026.10.2) :

| Configuration | RAM v0.6 | RAM v0.7 | Flash v0.6 | Flash v0.7 |
|---|---|---|---|---|
| Production | 38 548 o | 39 076 o (+528, 47,7 %) | 481 233 o | 484 461 o |
| Essai consigne ECS | 38 644 o | 39 172 o | 481 329 o | 484 589 o |
| Essai chauffage + climate M2 | — | 40 208 o (49,1 %) | — | 494 245 o |
| Fixture v0.4.1 (`make compat`) | 36 224 o | 36 756 o | 460 109 o | — |

La RAM statique ajoutée vient surtout des motifs de refus en anglais et des
tables de limites, que l'ESP8266 garde en RAM. Les transactions de consigne
sont allouées seulement pour les consignes déclarées.

## 5. Procédure d'essai sur la chaudière

Configuration :
[`iot-viessmann-essai-chauffage-v0.7.yaml`](../examples/iot-viessmann-essai-chauffage-v0.7.yaml).
Toutes les autorisations sont à `false`. Noter **sur papier** les quatre
consignes affichées par le panneau avant de commencer.

1. **Contrôler** les quatre consignes : Home Assistant = panneau. Relever
   les trames (`0x2306 <- 14` = 20 °C).
2. **Panneau** : modifier une consigne (+1 °C), par exemple la réduite A1.
3. **Relecture** : au plus 60 s après, l'entité suit et l'état affiche
   « Modifiée hors de Home Assistant ».
4. **Restaurer** la valeur initiale sur le panneau ; vérifier la relecture.
   Faire de même depuis la **Vitotrol 200** pour M2.
5. **Autoriser** (après accord explicite) : `enable_writes: true` et
   `enable_heating_setpoint_writes: true`, **rien d'autre**. Compiler :
   `HEATING SETPOINT WRITES ENABLED`, et aucun autre `… ENABLED`. Flasher.
6. **Modifier de 1 °C une seule consigne**, en commençant par `0x2307` (A1
   réduite, hors période réduite) : effet le plus limité.
7. **Journal** : relecture préalable, `0x2307 -> XX (write)`, accusé
   (`write acknowledged` ou `rejected`), relecture, « Confirmée ».
8. **Panneau** : la nouvelle valeur est affichée.
9. **Restaurer** la valeur initiale depuis Home Assistant ; « Confirmée » ;
   panneau conforme.
10. **Répéter séparément** pour `0x2306`, puis `0x3307` et `0x3306` (M2,
    écriture non documentée : surveiller particulièrement l'accusé et la
    relecture). Un registre par séance, au moins 30 min d'observation du
    départ M2 (`0x3900`) après chaque changement de M2.

Au moindre écart (« Échec », « Indéterminé », panneau différent) : ne plus
rien demander, remettre les autorisations à `false` et rétablir la valeur
**sur le panneau** depuis le relevé papier.

Le climate M2 se teste ensuite, toujours sans VTherm :

1. lecture (cible, mode, action) ;
2. `climate.set_temperature` (+1 °C) ;
3. retour à la valeur initiale ;
4. modes HEAT et OFF, seulement si `enable_mode_writes` et
   `enable_m2_mode_write` sont déjà validés pour M2.

## 6. Points restant à valider sur la KW3

1. **Écriture de `0x2306`/`0x2307`** : acceptation, forme de l'accusé.
2. **Écriture de `0x3306`/`0x3307`** : **aucune source**. Le registre peut
   être en lecture seule, ou écrit sans effet (relecture différente).
3. La régulation borne-t-elle les valeurs, ou impose-t-elle confort ≥
   réduite ?
4. Délai de prise en compte d'un changement au panneau et à la Vitotrol.
5. **`0x0898`** : ambiance mesurée par la Vitotrol 200 de M2.
   - *Observé* : la valeur lue est cohérente avec la température
     intérieure affichée par la Vitotrol.
   - *Reste à valider* : suivi dans la durée (montée et descente), codage
     (int16, 0,1 °C supposé), valeur sans Vitotrol ou Vitotrol
     déconnectée (`FF FF` ?).
6. **`0x3906`** : circulateur M2.
   - *Observé* : un état de circulateur est remonté.
   - *Reste à valider* : valeurs `00`/`01` seulement, et concordance avec
     le circulateur réel (marche/arrêt constatés sur place, y compris en
     programme `00`).
7. Effet d'un changement de consigne M2 sur le départ (`0x3900`) et durée
   de stabilisation.
8. Persistance après une coupure de la chaudière.
9. Limites internes définitives par circuit, avec l'installateur.
