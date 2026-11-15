# Écriture KW, consignes et programmes de fonctionnement (v0.3 – v0.4.1)

> **Statut : aucune écriture n'a été faite sur la chaudière.** Le code
> d'écriture est implémenté et testé **en simulation seulement**. Une
> simulation réussie montre que VissLink se comporte comme prévu face au
> comportement *supposé* de la régulation. Elle ne prouve pas que la
> Vitotronic 300 KW3 accepte ces écritures. Les écritures sont **désactivées
> par défaut** (`enable_writes: false`) et doivent le rester jusqu'à la fin de
> la procédure du § 10.

## 1. Sources

| Source | Version consultée | Apport |
|---|---|---|
| Wiki openv, « Protokoll KW » | 2026-11 | trame d'écriture `F4`, accusé `00` (« Bestätigung OK »), synchro `0x05` |
| Wiki openv, « Geräte » | 2026-11 | Vitotronic 300 KW3 = `V300KW3`, identifiant `209C`, protocole KW |
| Wiki openv, « Adressen » | 2026-11 | `0x23..` (A1) et `0x33..` (M2) en « Bedienung (read write) » ; plages 3..37 °C et 10..60 °C |
| vcontrold, `xml/kw/vcontrold.xml` et `xml/kw/vito.xml` | commit `f66b465` | `setaddr` = `SEND 01 F4 $addr $hexlen; SEND BYTES; RECV 1 SR` ; `SR` : `00` OK, `05` « SYNC (NOT OK) », autre « NOT OK » ; `setTempRaumNorSollM1` (`0x2306`), `setTempRaumRedSollM1` (`0x2307`), `setTempWWsoll` (`0x6300`) pour la **Vitotronic 200 KW2** (`2098`) |
| VitoWiFi (bertmelis), `src/VS1/` | commit `edc059a` | trame d'écriture `F4 AH AL LEN DATA` après `0x01` ; attend **LEN** octets de réponse |
| vcontrold, `xml/kw/vito.xml` (programmes) | commit `f66b465` | `getBetriebArtM1` / `setBetriebArtM1` (`0x2301`, 1 octet, unité `BA`), `getBetriebArtM2` (`0x3301`) **sans** `set` ; `BA` : `00` WW, `01` RED, `02` NORM, `03`/`04` H+WW, `05` ABSCHALT |
| Wiki openv, « Adressen » (programmes) | 2026-11 | « Betriebsart » `0x2301` dans la section « Bedienung (read write) » ; note pour un appareil : « 0x2301 cannot wite :-( 0x2323 read+write ok :-) » |
| Forum ioBroker (adaptateur Viessmann) | — | utilisation de `setBetriebArtM1` avec vcontrold ; valeurs à respecter exactement selon l'énumération |

Liens : <https://forum.iobroker.net/topic/2715/viessmann-adapter/76>,
<https://forum.iobroker.net/post/50636>,
<https://github.com/openv/openv/wiki/Protokoll-KW>,
<https://github.com/openv/openv/wiki/Ger%C3%A4te>,
<https://github.com/openv/openv/wiki/Adressen>,
<https://github.com/openv/vcontrold>, <https://github.com/bertmelis/VitoWiFi>.

Aucune documentation du fabricant n'a été trouvée pour le protocole Optolink.
Toutes ces sources sont communautaires (rétro-ingénierie).

## 2. Protocole retenu

### Lecture (inchangée depuis la v0.1)

```text
Vitotronic -> 05
VissLink   -> 01 F7 AH AL LEN
Vitotronic -> LEN octets de données
```

### Écriture (v0.3)

```text
Vitotronic -> 05
VissLink   -> 01 F4 AH AL LEN D1..DLEN       (une seule émission, juste après le 05)
Vitotronic -> 1 octet : 00 = acceptée
```

Exemple : consigne A1 normale (`0x2306`) à 21 °C → `01 F4 23 06 01 15`.

| | Lecture | Écriture |
|---|---|---|
| Commande | `F7` (« Virtuell_Read ») | `F4` (« Virtuell_Write ») |
| Données envoyées | aucune | LEN octets après la longueur |
| Réponse | LEN octets de données | **1 octet** d'accusé (vcontrold, openv) |
| Contrôle possible | cadrage seulement (aucune somme de contrôle) | valeur de l'accusé, puis **relecture** |
| Nouvelle tentative automatique | oui (`max_retries`) | **jamais** |

### Comportement implémenté

- **Synchronisation** : identique à la lecture. La trame part uniquement sur
  un `0x05` frais (dernier octet reçu, tampon vide), sinon elle n'est pas
  émise. Sans `0x05` pendant `sync_timeout`, l'écriture échoue en
  `sync timeout` et **aucun octet n'a été envoyé**.
- **Accusé** : `00` (puis 25 ms de silence) = acceptée. `05` ou toute autre
  valeur = `write rejected`, octet journalisé (codage vcontrold). Plus d'un
  octet = `response too long`. Rien pendant `response_timeout` = `response
  timeout`. Dans ces trois derniers cas, la ligne est resynchronisée
  (`RECOVERY`, 200 ms de silence) comme après une erreur de lecture.
- **Longueur** : **un octet** pour les consignes et les programmes. Pour une
  écriture de N > 1 octets, VitoWiFi attend N octets de réponse et vcontrold
  un seul. Les sources ne s'accordent que pour N = 1, et toutes les
  consignes visées sont des uint8. v0.5 : seule exception, les blocs
  horaires du bouclage (8 octets), dans un chemin expérimental isolé où
  toute réponse autre que `00` est une issue indéterminée tranchée par
  relecture (`docs/bouclage-ecs-v0.5.md`).
- **Après toute tentative** (acceptée, refusée, sans réponse) : relecture
  immédiate et prioritaire du registre, puis comparaison avec la valeur
  demandée. Cette relecture est le seul moyen fiable de savoir si la
  consigne a changé.

### Points non confirmés (non utilisés ou traités prudemment)

| Point | Sources | Traitement dans VissLink |
|---|---|---|
| Réponse d'une écriture de plusieurs octets | divergentes | écriture limitée à 1 octet |
| Réaction de la KW3 à une écriture refusée (adresse en lecture seule, valeur hors plage) | `05` selon vcontrold, non observé | toute réponse ≠ `00` = refus ; relecture |
| Délai de réponse d'une écriture (enregistrement interne) | non documenté (« xx msec ») | `response_timeout` (1 s) ; une réponse tardive est vidée pendant la reprise |
| Enchaînement d'une trame sans attendre de nouveau `0x05` (openv : sans `0x01`) | openv seul | **non utilisé** : chaque transaction attend son `0x05` |
| `04` (EOT) avant l'attente du `0x05` (vcontrold, VitoWiFi : remise en mode KW après P300) | deux implémentations | **non utilisé** : la régulation est déjà en KW (lectures fonctionnelles) |
| Comportement d'une trame d'écriture tronquée | non documenté | voir § 7 |
| Plages acceptées par la KW3 elle-même | openv (tableau générique), non spécifique à la KW3 | bornes doublées : documentées dans le code + bornes YAML plus étroites |

## 3. KW et P300 : ne pas confondre

| | KW (VS1) — utilisé | P300 (VS2) — **non utilisé** |
|---|---|---|
| Liaison | 4800 bauds 8E2 | 4800 bauds 8E2 |
| Initialisation | aucune : la régulation émet `05` en continu | `04`, attente `05`, puis `16 00 00` → `06` |
| Cadence | une transaction par `05` | session permanente, pas de `05` |
| Trame | `01 F7/F4 AH AL LEN [D…]`, sans en-tête ni somme de contrôle | `41 LEN TYPE FONCTION AH AL N [D…] CRC` |
| Écriture | `F4` | fonction `02` |
| Accusé | réponse `00` (1 octet) | `06` puis trame de réponse ; `15` = NACK |

VissLink n'implémente que KW. Aucun élément P300 (en-tête `41`, somme de
contrôle, `06`/`15`) n'est utilisé.

## 4. Registres

| Adresse | Fonction | 1. Lecture confirmée | 2. Écriture documentée | 3. Écriture testée sur chaudière |
|---|---|---|---|---|
| `0x2306` | A1, consigne d'ambiance normale (confort) | **oui** | oui : vcontrold (V200KW2), openv (A1 « read write », 3..37) | **non** |
| `0x2307` | A1, consigne d'ambiance réduite (éco) | **oui** | oui : vcontrold (V200KW2), openv (3..37) | **non** |
| `0x3306` | M2, consigne d'ambiance normale | **oui** | **par analogie seulement** : openv « M2 : 0x33.. » ; aucun `set` dans vcontrold | **non** |
| `0x3307` | M2, consigne d'ambiance réduite | **oui** | **par analogie seulement** (idem) | **non** |
| `0x6300` | consigne d'eau chaude sanitaire | **oui** | oui : vcontrold (V200KW2), openv (10..60) | **non** |

Aucune source ne publie de liste d'adresses propre à la Vitotronic 300 KW3
(`209C`). Toutes les écritures « documentées » le sont pour une autre
régulation KW (200 KW2) ou par un tableau générique. Elles restent à
confirmer sur la chaudière (§ 10). Une lecture réussie ne prouve rien sur
l'écriture.

Toute autre adresse est **refusée** : températures, brûleur, compteurs,
programmations, registres expérimentaux, et les voisines des consignes
(`0x2308` party, `0x2302`/`0x2303` modes…).

## 5. Interface Home Assistant

```yaml
visslink:
  id: viessmann
  uart_id: optolink_uart
  enable_writes: false      # défaut ; true seulement après le § 10

number:
  - platform: visslink
    visslink_id: viessmann
    name: "Radiateurs - Consigne confort"
    address: 0x2306         # liste blanche uniquement
    min_value: 16           # obligatoire, entier, >= borne documentée
    max_value: 24           # obligatoire, entier, <= borne documentée
    step: 1                 # 1 uniquement (défaut)
    unit_of_measurement: "°C"
    mode: box               # option standard ESPHome (auto, box, slider)
    update_interval: 5min   # défaut
```

Options propres à VissLink : `address`, `min_value`, `max_value`, `step`,
`update_interval`, `visslink_id`. Les autres options (`name`,
`unit_of_measurement`, `mode`, `icon`, `entity_category`…) sont celles de
toute entité `number` ESPHome. `restore_value`, `initial_value` et
`data_type` sont refusés.

- L'état publié est **toujours la valeur lue** : lecture toutes les 5 min et
  relecture juste après une écriture. La valeur demandée n'est jamais
  publiée.
- Une demande refusée (écritures désactivées, valeur invalide, liaison
  absente…) est journalisée et l'entité republie la valeur lue, si bien que
  Home Assistant revient à l'état réel.
- Une adresse ne peut être déclarée qu'une fois, toutes plateformes
  confondues : une consigne en `number` **remplace** son ancien `sensor`.
  Déclarer les deux est refusé à la validation (`Register 0x2306 is declared
  by several visslink entities`). Dans l'exemple v0.3, les cinq `sensor` de
  consigne ont été retirés (un test le vérifie).

### Migration Home Assistant (v0.2.x → v0.3)

| v0.2.x (`sensor`) | v0.3 (`number`) |
|---|---|
| Consigne A1 confort (`0x2306`) | Radiateurs - Consigne confort |
| Consigne A1 réduit (`0x2307`) | Radiateurs - Consigne réduite |
| Consigne M2 confort (`0x3306`) | Plancher - Consigne confort |
| Consigne M2 réduit (`0x3307`) | Plancher - Consigne réduite |
| Consigne ECS (`0x6300`) | ECS - Consigne |

Les identifiants d'entité changent de domaine (`sensor.…` → `number.…`) : il
n'y a pas de migration automatique.

1. Après le flash de la v0.3, les cinq anciennes entités `sensor` sont
   « indisponibles » (non fournies par l'appareil). Les supprimer dans
   *Paramètres → Appareils et services → ESPHome → appareil → Entités*.
2. Mettre à jour tableaux de bord, automatisations et scripts qui
   utilisaient ces `sensor.…`.
3. Historique : celui des anciens capteurs reste dans la base jusqu'à sa
   purge, mais n'est pas rattaché aux nouvelles entités. Une entité `number`
   n'a pas de statistiques à long terme. Si un graphe durable des consignes
   est utile, créer dans Home Assistant un capteur modèle (`template`) qui
   recopie la valeur du `number` avec `state_class: measurement`.
4. Écritures toujours désactivées : modifier un `number` dans Home Assistant
   produit un avertissement dans le journal et l'entité revient à la valeur
   lue.
5. Revenir en v0.2.2 = reflasher l'ancien YAML ; les `number` deviennent à
   leur tour orphelins.

## 6. Protections

| Exigence | Mise en œuvre |
|---|---|
| Désactivées par défaut, activation explicite | `enable_writes` (défaut `false`) ; avertissement `WRITES ENABLED` à la validation ; sinon toute demande est refusée avant l'arbitre |
| Liste blanche | `number.py` (validation YAML) **et** `write_guard.h` (vérifié au dépôt **et** juste avant l'émission) ; un test vérifie que les deux listes sont identiques et contiennent exactement `0x2306`, `0x2307`, `0x3306`, `0x3307`, `0x6300` |
| Contrôle avant émission | `VissLinkRegister::write_allowed()` refuse par défaut ; seul `VissLinkNumber` l'accepte après un contrôle complet (liste blanche, 1 octet, bornes du YAML **et** plage documentée) refait juste avant `request_write` ; test `sim_v03_send_time_check_blocks_corrupted_requests` |
| Longueur et type | uint8, 1 octet, contrôlé par la liste blanche ; le moteur KW n'accepte que 1 octet, ou 8 pour les blocs horaires du bouclage (v0.5, chemin expérimental isolé, `docs/bouclage-ecs-v0.5.md`) |
| Valeurs | consignes : finie, entière, dans `min_value..max_value` (déjà refusé par ESPHome au-delà) **et** dans la plage documentée ; programmes : `00`, `03`, `05` exclusivement |
| Programmes de fonctionnement | `enable_mode_writes` (défaut `false`, exige `enable_writes`) ; M2 : en plus `enable_m2_mode_write` (défaut `false`, exige les deux autres) ; autorisations revérifiées à la demande **et** juste avant l'émission ; liste blanche distincte (`0x2301`, `0x3301`) ; une commande de programme à la fois ; relecture de A1 **et** M2 ; divergence affichée sans correction (§ 9) |
| Adresse inconnue | refusée à la configuration, au dépôt et à l'émission |
| Pas d'écriture au démarrage | aucune restauration (`restore_value`, `initial_value` refusés ; aucune lecture de préférences en flash), aucune file persistante : la file est en RAM et vide au démarrage ; une demande est en outre refusée tant que la consigne n'a pas été lue et que la liaison n'est pas synchronisée ; seule une action Home Assistant (`control()`) crée une demande |
| Pas d'écriture à la reconnexion de Home Assistant | à la reconnexion, Home Assistant s'abonne aux états mais n'appelle pas `number.set_value` ; l'état qu'il restaure à son propre démarrage n'est qu'affiché, jamais renvoyé ; VissLink ne rejoue rien |
| Pas de répétition infinie | **aucune** nouvelle tentative d'écriture ; seule la relecture est retentée (`max_retries`) |
| Pas de rafale | une écriture à la fois (envoi → relecture terminée), puis 10 s minimum (`MIN_SPACING_MS`) avant la suivante |
| File bornée | une demande en attente par entité (6 au plus : 5 consignes + 1 programme) ; une nouvelle demande sur la même consigne **remplace** l'ancienne non encore envoyée ; pour un programme, elle est **refusée** |
| Pas pendant une transaction | le moteur KW refuse toute requête hors de l'état `IDLE` ; l'écriture n'est lancée que si aucune lecture n'est en cours |
| Priorité sans interruption (v0.4.1) | une commande prête ne remplace qu'une lecture ordinaire **pas encore émise** (attente du `0x05`) ; jamais une transaction dont la trame est partie, ni une relecture ; pendant les relectures, aucune lecture ordinaire (§ 14) |
| Liaison désynchronisée | refus au dépôt et à l'émission si la dernière transaction a échoué sur un `sync timeout` ou si aucune lecture n'a encore réussi ; refus si la consigne n'a jamais été lue |
| Demande périmée | abandonnée si elle n'est pas partie après 120 s (`REQUEST_EXPIRY_MS`) ; jamais envoyée plus tard |
| Valeur inchangée | demande égale à la valeur lue : rien n'est écrit (usure de la mémoire de la régulation) |
| Contrôles de la Vitotronic | jamais contournés : un refus ou une valeur bornée par la régulation est constaté par la relecture et publié tel quel |

Une perte du Wi-Fi ou de Home Assistant ne crée aucune demande et n'en
modifie aucune. Une demande déjà acceptée juste avant la coupure part
normalement, au plus 120 s plus tard. Sinon, elle est abandonnée. La
chaudière continue de fonctionner avec ses propres consignes : VissLink ne
lui envoie rien de plus que des lectures.

### Séquence d'une écriture

1. Home Assistant demande une valeur (`number.set_value`).
2. Contrôles : écritures activées, liste blanche, valeur, consigne déjà lue,
   liaison synchronisée. En cas de refus : journal `write of X refused: …`,
   puis republication de la valeur lue.
3. Mise en file (ou remplacement de la demande en attente).
4. Dès que la ligne est libre (aucune lecture en cours), que la liaison est
   bonne et que l'espacement est respecté, la demande la plus ancienne part :
   journal `0x2306 -> 15 (write)`.
5. Accusé : `0x2306 <- 00 (write acknowledged)`, ou `write rejected (reply
   05)`, ou `response timeout, outcome indeterminate until read-back (not resent)`.
6. Relecture prioritaire du registre (avant toute autre lecture échue).
7. Comparaison : `write confirmed by read-back (21)`, ou `write NOT applied:
   read back 20, requested 21`, ou `write indeterminate (unverified): read-back failed` (état
   inconnu).
8. Publication de la valeur **lue**. La ligne revient aux lectures pour au
   moins 10 s.

## 7. Écriture interrompue

| Situation | Conséquence | Comportement VissLink |
|---|---|---|
| Pas de `0x05` (interface débranchée) | rien n'est émis | `sync timeout`, relecture (abandonnée si la liaison reste coupée), aucune nouvelle tentative |
| Accusé perdu ou illisible | **issue indéterminée** : la consigne a pu changer | jamais réémise ; l'entité garde la dernière valeur **lue** ; relecture : confirmée, `NOT applied`, ou `indeterminate (unverified)` si la relecture échoue aussi (état publié : inconnu) ; la valeur réelle est apprise à la lecture suivante (test `sim_v03_lost_ack_is_indeterminate_until_readback`) |
| Accusé `00` mais valeur non appliquée ou bornée | la régulation a gardé ou corrigé la valeur | relecture → `NOT applied`, publication de la valeur réelle |
| ESP redémarré pendant l'émission (trame tronquée, ex. `01 F4 23`) | comportement de la KW3 **non documenté** ; vraisemblablement abandon de la trame incomplète | au redémarrage, rien n'est rejoué ; première transaction = lecture |
| Coupure secteur de la chaudière pendant l'enregistrement | interne à la régulation, non documenté | aucune action possible : relecture au retour de la liaison |

Une écriture interrompue ne peut donc pas aboutir à une valeur **hors
plage** envoyée par VissLink : seule une valeur entière validée est émise, en
une seule écriture d'un octet. Le risque résiduel est qu'une consigne valide
soit appliquée sans que VissLink le sache immédiatement. La relecture
périodique (5 min) le corrige.

## 8. Production ECS ponctuelle : non implémentée

Aucune commande documentée n'a été trouvée pour déclencher une production
ECS ponctuelle sur la Vitotronic 300 KW3 via Optolink :

- vcontrold (KW2) et openv ne décrivent aucune adresse « einmalige
  Warmwasserbereitung » pour les régulations KW ;
- la fonction « Einmalige Warmwasserbereitung außerhalb des Zeitprogramms »
  apparaît dans des notices d'autres appareils (commande à distance
  Vitotrol 300-A, autres régulations), pas dans une source Optolink KW3 ;
- le mode « party » (`0x2303`, `0x3303`) existe en écriture pour la KW2, mais
  c'est un mode de **chauffage** dont l'effet sur l'eau chaude n'est pas
  documenté pour la KW3.

À ne pas confondre :

| Élément | Registre | Rôle |
|---|---|---|
| Consigne de température ECS | `0x6300` (whitelist) | température cible du ballon |
| Pompe de charge du ballon | `0x0845` (expérimental, lecture) | état d'un actionneur piloté par la régulation |
| Bouclage sanitaire | `0x0846` (expérimental, lecture) | état d'un actionneur piloté par la régulation |
| Programmation ECS | `0x2100..0x2137` (lecture) | plages horaires autorisées |
| Production ponctuelle | **inconnu** | fonction de la régulation, non identifiée |

VissLink n'actionne aucun registre de pompe et n'écrit pas dans les
programmations. Contournement possible sans nouvelle écriture : relever
temporairement `0x6300` (dans ses bornes), puis le rétablir. Cela agit sur la
consigne, pas sur la programmation : une charge n'aura lieu que pendant une
plage ECS active.

## 9. Programmes de fonctionnement (expérimental)

### Ce qui est établi

Valeurs **lues sur cette Vitotronic 300 KW3**, en `0x2301` (A1) comme en
`0x3301` (M2) :

| Valeur | Texte affiché |
|---|---|
| `00` | « ECS seule » |
| `03` | « Chauffage + ECS » |
| `05` | « Arrêt » |
| autre | **inconnu** : aucun mode n'est choisi arbitrairement (select : état inconnu ; text_sensor : « Inconnu ») |

Ce codage est identique à l'unité `BA` de vcontrold pour la 200 KW2 (`00` WW,
`03` H+WW, `05` ABSCHALT). Toute autre valeur lue (`01`, `02`, `04`, …) n'est
pas interprétée.

| Adresse | Lecture confirmée | Écriture documentée | Écriture testée | Autorisations requises |
|---|---|---|---|---|
| `0x2301` (A1) | **oui** (00, 03, 05) | **oui, mais contredite** : vcontrold `setBetriebArtM1` (200 KW2), utilisé par des utilisateurs ioBroker ; une note du wiki openv indique pour un autre appareil « 0x2301 cannot write, 0x2323 read+write ok » | **non** | `enable_writes` + `enable_mode_writes` |
| `0x3301` (M2) | **oui** (00, 03, 05) | **non** : vcontrold ne la déclare qu'en lecture (`getBetriebArtM2`) ; seule l'analogie avec A1 la suggère | **non** | `enable_writes` + `enable_mode_writes` + **`enable_m2_mode_write`** |

Le format de trame est le format d'écriture KW commun (§ 2). L'incertitude
porte sur l'**acceptation** de l'écriture par la KW3 à ces adresses. Pour M2,
l'écriture est rendue possible (v0.4) **à titre expérimental**, sans aucune
source : c'est la raison de la troisième autorisation.

### Mise en œuvre (v0.4)

```yaml
visslink:
  id: viessmann
  enable_writes: false          # défaut
  enable_mode_writes: false     # défaut ; exige enable_writes: true
  enable_m2_mode_write: false   # défaut ; exige les deux précédentes

select:
  - platform: visslink
    visslink_id: viessmann
    name: "Radiateurs - Mode actuel"
    address: 0x2301
    divergence:                 # facultatif, une seule fois pour A1/M2
      name: "Modes A1 et M2 différents"
  - platform: visslink
    visslink_id: viessmann
    name: "Plancher chauffant - Mode actuel"
    address: 0x3301
```

- **Deux sélecteurs indépendants**, un par circuit, options exactement
  « ECS seule », « Chauffage + ECS », « Arrêt ». Pas de doublon numérique.
- **Autorisations** (validées à la configuration, puis revérifiées à la
  demande et juste avant l'émission) :

  | `enable_writes` | `enable_mode_writes` | `enable_m2_mode_write` | A1 | M2 |
  |---|---|---|---|---|
  | false | — | — | refusé | refusé |
  | true | false | false | refusé | refusé |
  | true | true | false | **autorisé** | refusé |
  | true | true | true | **autorisé** | **autorisé** |
  | toute combinaison où `enable_m2_mode_write: true` sans les deux autres | | | **erreur de validation** | |

  `enable_mode_writes: true` sans `enable_writes: true` est aussi une erreur
  de validation. Les trois options valent `false` par défaut.
- Une commande refusée est journalisée (`M2 operating mode write is
  disabled (enable_m2_mode_write: false)`, etc.) et l'entité revient à la
  valeur lue. Le sélecteur M2 reste affiché comme modifiable dans Home
  Assistant (ESPHome n'a pas de select en lecture seule). Pour un affichage
  strictement non modifiable, déclarer M2 en `text_sensor` avec
  `type: operating_mode` (comme en v0.3, toujours accepté).
- **L'état d'un sélecteur est toujours la dernière valeur relue** : une
  commande acceptée ou refusée ne change pas l'option affichée. Elle est
  republiée aussitôt, et ne change qu'après la relecture.
- Liste blanche **distincte** de celle des consignes (`MODE_WRITE_WHITELIST`
  = `0x2301`, `0x3301`). Valeurs écrites : `00`, `03`, `05` exclusivement.
- **Une commande = un registre.** VissLink n'écrit jamais l'autre circuit à
  la suite, ni pour « corriger » une divergence.
- **Une seule commande de programme à la fois** : une nouvelle demande, sur
  le même circuit ou sur l'autre, est refusée (`another operating mode write
  is in progress`) tant que la précédente n'a pas été envoyée et ses deux
  relectures terminées. Elle n'est ni remplacée ni mise en file.
- **Relecture systématique des deux programmes** après chaque commande,
  quelle qu'en soit l'issue (accusé `00`, refus, sans réponse), avant toute
  autre lecture. L'écriture n'est terminée qu'après les deux relectures.
- **Un accusé `00` ne prouve rien** : seule la relecture du registre écrit
  détermine le programme publié (`confirmed`, `NOT applied`, ou
  `indeterminate (unverified)`).
- **Divergence** : si A1 et M2 diffèrent après la commande, le journal
  l'indique (`Operating modes differ after the write: … (not corrected
  automatically)`) et l'entité `divergence` passe à « on ». Elle est
  « inconnue » si l'un des deux programmes n'est pas lu. Les deux circuits
  peuvent légitimement avoir des programmes différents.
- Aucune nouvelle tentative, rien au démarrage, rien à la reconnexion
  (mêmes garanties que les consignes, § 6).
- **Party et Congé : non implémentés** (registres inconnus pour la KW3). Les
  candidats `0x2323` et `0x3323` restent à étudier **en lecture seule**. Si
  l'essai sur `0x2301` échoue (refus `05`, ou accusé sans effet), la note
  openv désigne `0x2323` comme piste, à confirmer d'abord par lecture.

Tests démontrant qu'aucune écriture M2 ne part sans la troisième
autorisation : `mode_write_permission_matrix`, `sim_v04_m2_permission_matrix`
(8 combinaisons), `sim_v04_m2_never_written_without_third_permission_after_reboot_reconnect`,
`sim_v04_m2_never_written_after_a1_timeout_or_concurrently`,
`sim_v04_m2_send_time_check_blocks_bypassed_requests` (demande injectée
directement dans l'arbitre, ou autorisation retirée avant l'émission).

### Essai matériel A (programme A1) : une seule écriture réversible

Prérequis : étapes 1 et 2 du § 10 (valeurs d'origine notées, identifiant
`20 9C`).

1. **Moment** : en journée, chauffage en marche normale, ballon chaud. A1 et
   M2 lus tous deux à « Chauffage + ECS » ; noter ce qu'affichent la
   Vitotronic **et la télécommande**.
2. **Configuration** : `enable_writes: true`, `enable_mode_writes: true`,
   **`enable_m2_mode_write: false`**. Garder les deux sélecteurs (double
   relecture, divergence). Ne laisser aucune consigne modifiable (`number`
   remplacés par les `sensor` de `tests/fixtures/iot-viessmann-v0.2.2.yaml`).
   Compiler et vérifier l'avertissement `OPERATING MODE WRITES ENABLED`, et
   l'**absence** de `M2 OPERATING MODE WRITE ENABLED`.
3. **Une seule écriture** : « Radiateurs - Mode actuel » de « Chauffage +
   ECS » (`03`) vers « ECS seule » (`00`). Ne pas commencer par « Arrêt ».
4. Journal attendu : `0x2301: operating mode 00 queued (experimental)`,
   `0x2301 -> 00 (write)`, `0x2301 <- 00 (write acknowledged)`, relectures
   `0x2301 <- 00` et `0x3301 <- 03` (ou `00` si la KW3 couple les circuits),
   `operating mode write confirmed by read-back (00)`, puis `Operating modes
   differ after the write: 0x2301 = 00, 0x3301 = 03 (not corrected
   automatically)` ou `both 00`.
5. **Comparer** : afficheur, **télécommande**, « Radiateurs - Mode actuel »,
   « Plancher chauffant - Mode actuel », « Modes A1 et M2 différents ». Noter
   si M2 a changé de lui-même.
6. **Revenir** dans les minutes qui suivent vers « Chauffage + ECS » (`03`),
   même vérification. En cas de doute, rétablir depuis la régulation ou la
   télécommande.
7. **Au moindre écart** (réponse ≠ `00`, `response timeout`, `NOT
   applied`, afficheur ou télécommande non mis à jour, M2 modifié de façon
   inattendue) : `enable_mode_writes: false` et analyser.
8. Un autre jour seulement : « Arrêt » (`05`) puis retour à `03`.

**A1 est validé** quand l'accusé, la relecture, l'afficheur et la
télécommande concordent, à l'aller comme au retour.

### Essai matériel B (programme M2) : uniquement après la validation de A1

M2 n'a **aucune** source documentant son écriture. Cet essai n'a de sens
qu'une fois A1 validé (essai A) : le format et le comportement de l'accusé
sont alors connus sur cette KW3.

1. **Prérequis** : essai A réussi et noté. Un autre jour que l'essai A.
   Plancher chauffant en marche normale ; M2 et A1 lus à « Chauffage +
   ECS ». Noter afficheur et télécommande **pour M2**.
2. **Configuration** : les trois options à `true`. Aucune consigne
   modifiable. Compiler et vérifier les deux avertissements
   `OPERATING MODE WRITES ENABLED` et `M2 OPERATING MODE WRITE ENABLED`.
3. **Ne rien commander sur A1 pendant l'essai.**
4. **Une seule écriture** : « Plancher chauffant - Mode actuel » de
   « Chauffage + ECS » (`03`) vers « ECS seule » (`00`).
5. Journal attendu : `0x3301 -> 00 (write)`, puis soit
   `0x3301 <- 00 (write acknowledged)`, soit `write rejected (reply 05)`, soit
   `response timeout, outcome indeterminate until read-back (not resent)`.
   Ensuite, relectures `0x3301` et `0x2301`, puis `confirmed`, `NOT applied`
   ou `indeterminate`, et l'état de la divergence.
6. **Comparer** : afficheur et télécommande pour M2 **et** pour A1 (A1 ne
   doit pas avoir changé), entités et divergence.
7. **Revenir** vers « Chauffage + ECS » (`03`) depuis Home Assistant si
   l'écriture a été confirmée. Sinon, vérifier sur la régulation que M2 est
   resté à « Chauffage + ECS ».
8. **Résultats possibles** :
   - refus `05`, ou accusé sans effet (`NOT applied`) : l'écriture M2 n'est
     pas acceptée à cette adresse. `enable_m2_mode_write: false`
     définitivement, pas d'autre essai « pour voir » ;
   - confirmé, et afficheur et télécommande concordent : écriture M2
     constatée sur cette installation. La reporter dans le tableau ci-dessus
     (« Écriture testée : oui, date ») ;
   - autre écart (A1 modifié, afficheur incohérent) : `enable_m2_mode_write:
     false` et analyse.
9. « Arrêt » (`05`) sur M2 seulement un autre jour, après un retour réussi.

## 10. Validation avant activation des écritures (sur la chaudière)

À faire sur place, devant la Vitotronic, avec la notice et les valeurs
d'origine notées.

1. **Préparation** : noter les cinq consignes affichées par la régulation et
   savoir les rétablir depuis son panneau. Vérifier que les cinq `number`
   affichent ces mêmes valeurs (lecture, écritures encore désactivées).
2. **Identifier la régulation** (lecture seule) : déclarer temporairement un
   `sensor` `address: 0x00F8`, `data_type: uint16`, et vérifier dans le
   journal `0x00F8 <- 20 9C`, l'identifiant KW3 d'openv. Autre valeur :
   arrêter.
3. **Un seul registre d'abord** : ne garder qu'un `number`, de préférence
   A1 réduit (`0x2307`), hors période de mode réduit. Laisser les autres
   consignes en `sensor`.
4. Passer `enable_writes: true`, compiler, flasher (vérifier l'avertissement
   `WRITES ENABLED` à la compilation).
5. Depuis Home Assistant, modifier la consigne de **+1 °C**. Attendu dans le
   journal : `-> (write)`, `<- 00 (write acknowledged)`, `write confirmed by
   read-back`. Vérifier **sur l'afficheur** de la Vitotronic.
6. Rétablir la valeur d'origine depuis Home Assistant et la vérifier sur
   l'afficheur.
7. Noter tout écart : réponse différente de `00`, absence de réponse, délai,
   valeur relue différente, afficheur non mis à jour. **Au moindre écart :
   `enable_writes: false` et analyser avant de continuer.**
8. Recommencer, un registre à la fois : `0x2306`, `0x6300`, puis
   `0x3307` et `0x3306`. Pour M2, l'écriture n'est documentée que par
   analogie : surveiller tout particulièrement l'afficheur.
9. Optionnel, sans risque : régler une valeur hors des bornes du YAML
   depuis Home Assistant. Elle doit être refusée par ESPHome, sans aucune
   trame `F4` dans le journal.
10. Reporter les résultats (date, valeur, accusé, relecture, afficheur) dans
    la colonne « Écriture testée » du § 4. Ensuite seulement, réactiver
    les consignes voulues.

Ne jamais essayer d'adresse hors liste blanche, ni de valeur hors plage
documentée, « pour voir ».

## 11. Charge de la liaison (simulation)

Une écriture coûte deux transactions : l'écriture et sa relecture. En usage
normal (quelques modifications par jour), l'effet sur les lectures est
négligeable.

Pire cas simulé (`sim_v03_write_spam_keeps_periodic_reads`) : une demande
toutes les 2 s pendant 6 h 30, sur les cinq consignes, avec les
programmations. L'espacement plafonne alors les écritures à une toutes les
~15 s (v0.4.1 : 1 470 à 1 799 écritures).

| Resynchro | Brûleur (30 s) : lectures perdues / 780, écart max — v0.4 | — v0.4.1 (commandes prioritaires) | Écart max température extérieure (v0.4.1) |
|---|---|---|---|
| 2 s | 0, 39 s | 0, 38 s | 64 s |
| 3 s | 1, 50 s | 1, 53 s | 79 s |
| 4 s | 18, 85 s | 42, 84 s | 97 s |

La priorité donnée aux commandes (§ 14) les fait partir plus vite, donc plus
souvent dans ce scénario irréaliste. À 4 s de resynchro, le brûleur perd
davantage de lectures, sans jamais sauter plus de deux cycles. L'espacement
minimal de 10 s entre deux écritures est inchangé. En usage réel (une
commande A1 toutes les 47 s, encore très au-delà d'un usage normal), le
brûleur n'est pas pénalisé (§ 14).

Les programmations restent lues intégralement. Aucune requête n'est
dupliquée et deux écritures ne se suivent jamais sans relecture.

## 12. Mémoire (Wemos D1 mini, ESPHome 2026.10.2)

| Configuration | v0.2.2 : RAM / Flash | v0.4 : RAM / Flash |
|---|---|---|
| YAML v0.1 (inchangé) | 31 256 / 421 289 o | 31 588 (+332) / 424 069 (+2 780) o |
| YAML v0.2 (inchangé) | 31 980 / 424 353 o | 32 312 (+332) / 427 181 (+2 828) o |
| YAML v0.2.1 / v0.2.2 (inchangé) | 33 796 / 434 713 o | 34 160 (+364) / 437 557 (+2 844) o |
| Diagnostic v0.2.2 (inchangé) | 34 304 / 435 517 o | 34 676 (+372) / 438 377 (+2 860) o |
| YAML v0.3 (inchangé : 5 `number`, select A1, text_sensor M2) | — | 35 292 (43,1 %) / 447 885 (42,9 %) o |
| **Exemple v0.4** (5 `number`, 2 `select`) | — | **35 252 (43,0 %) / 447 313 (42,8 %) o** |
| **Exemple v0.4.1** (même YAML, réactivité) | — | **35 284 (43,1 %) / 447 985 (42,9 %) o** (+32 o / +672 o) |

Le moteur d'écriture, l'arbitre et les listes blanches coûtent ~350 o de RAM
et ~2,8 Ko de Flash, même sans `number` ni `select`. Les trois options
d'autorisation ne changent pas l'empreinte : à peine 16 o de Flash entre
« tout désactivé » et « A1 + M2 autorisés ».

## 13. Inventaire avant les essais matériels

- [ ] Identifiant de la régulation lu en `0x00F8` (attendu `20 9C`).
- [ ] Valeurs d'origine des cinq consignes notées sur l'afficheur.
- [ ] Bornes `min_value` / `max_value` choisies pour l'installation
      (plancher chauffant : vérifier les limites de la dalle et du
      réglage ; ECS : prévention légionelle ⇒ éviter une consigne basse
      prolongée).
- [ ] Confirmation de l'accusé `00` sur la KW3 (étape 5).
- [ ] Réaction de la KW3 à un refus : inconnue, à observer seulement si
      elle survient naturellement.
- [ ] Délai réel de l'accusé (journal horodaté), inférieur à 1 s ?
- [ ] Confirmation que l'afficheur et la régulation prennent la nouvelle
      consigne en compte.
- [ ] Validation séparée de `0x3306` / `0x3307` (M2, analogie seulement).
- [x] Programmes : valeurs `00`, `03`, `05` lues en `0x2301` et `0x3301`.
- [ ] Programme A1 : essai d'écriture réversible `03` → `00` → `03` (§ 9),
      comparaison afficheur / télécommande, comportement de M2.
- [ ] Programme M2 : essai B (§ 9), seulement après la validation de A1 ;
      `enable_m2_mode_write: false` en dehors de l'essai.
- [ ] Party, Congé, `0x2323` / `0x3323` : identification par lecture seule.
- [ ] Production ECS ponctuelle : identifier une fonction documentée de la
      KW3, faute de quoi elle reste non implémentée.
- [ ] Décider ensuite, registre par registre, du passage à
      `enable_writes: true` en production.

## 14. Réactivité des commandes (v0.4.1)

### Analyse du délai (v0.4)

La priorité des commandes existait déjà en v0.4. Dès que la ligne est
libre, une écriture en attente passe avant toute lecture périodique,
programmations horaires comprises, et ses relectures sont urgentes. Le
délai provenait de :

1. **L'attente du `0x05`, pour chaque transaction.** Après chaque réponse,
   la Vitotronic ne renvoie un `0x05` qu'après 2 à 4 s. Une commande en
   demande au moins deux : l'écriture, puis la relecture qui publie l'état
   (une troisième pour l'autre circuit, après un programme). C'est la part
   principale, **incompressible** tant qu'on respecte la synchronisation.
2. **Une lecture périodique déjà acquise mais pas encore émise** (en attente
   de son `0x05`) passait avant la commande : une synchronisation entière de
   plus (2 à 4 s, jusqu'à `sync_timeout` si la chaudière se tait). Pendant
   la lecture des programmations, la ligne est presque toujours dans ce cas.
3. **L'ordre des relectures** suivait l'ordre de déclaration. Pour une
   commande M2, A1 était relu avant M2 : l'état de M2 était publié une
   synchronisation plus tard.
4. **Une relecture en échec** laissait passer une lecture ordinaire pendant
   son délai de nouvelle tentative (1 s).

### Modifications (v0.4.1)

| Point | Changement |
|---|---|
| 2 | Une commande prête **prend la place d'une lecture ordinaire en attente du `0x05`**. Rien n'a été émis pour cette lecture (`KwProtocol::cancel_before_send()` n'agit que dans l'état `WAIT_SYNC`), qui garde son échéance et repasse juste après. Une transaction dont la trame est partie (réponse attendue, fenêtre de silence, reprise) **n'est jamais interrompue**, ni une relecture de vérification. La commande attend ensuite un `0x05` frais, comme toute requête. |
| 3 | Le registre écrit est **relu en premier**, puis l'autre circuit. |
| 4 | Tant que des relectures de vérification sont en cours, **aucune lecture ordinaire** n'est démarrée (`acquire(now, urgent_only)`), même pendant un délai de nouvelle tentative. |
| 1 | Inchangé. Aucun délai raccourci : `sync_timeout`, `response_timeout`, fenêtre de silence, reprise, espacement de 10 s entre écritures. L'enchaînement d'une trame sans attendre de `0x05` (openv) reste **non utilisé**, faute de confirmation. |

Inchangés aussi : écritures désactivées par défaut, listes blanches,
autorisations, une écriture à la fois, aucune réémission, demandes périmées
abandonnées.

### Journal de mesure (sur la chaudière)

Chaque écriture se termine par une ligne de chronologie :

```text
[I][visslink]: 0x2301: write timing: line wait 12 ms, sync wait 1830 ms, send+ack 70 ms,
               read-back 3045 ms; state published 4957 ms after the request, all read-backs 8002 ms
```

| Champ | Mesure | Contenu |
|---|---|---|
| `line wait` | demande Home Assistant → début de la transaction KW | fin de la trame éventuellement en cours ; 0 si la ligne est libre ou si une lecture non émise a été préemptée (journal DEBUG `periodic read deferred for a manual command`) |
| `sync wait` | début → émission de la trame | **attente du `0x05`** |
| `send+ack` | émission → accusé | **transmission** (6 octets, ~15 ms à 4800 bauds 8E2), réponse de la régulation, 25 ms de silence de contrôle |
| `read-back` | accusé → relecture du registre écrit | **relecture**, dont son propre `0x05` (le plus long) |
| `state published` | demande → état publié dans Home Assistant | total vu par l'utilisateur |
| `all read-backs` | demande → fin des relectures | ajoute la relecture de l'autre circuit (programmes) |

Le délai entre l'action dans Home Assistant et l'arrivée de la demande sur
l'ESP (réseau, API) n'est pas compté ; il est normalement négligeable.

### Temps de réponse simulés

Chaudière simulée, une commande A1 toutes les 47 s pendant 6 h 20, avec
brûleur (30 s), températures (60 s), consignes, programmes et les
4 programmations horaires (lues au démarrage et à 6 h). Test
`sim_v041_command_latency_under_load`. Moyenne / maximum, en ms :

| Resynchro | Version | Attente de la ligne | Attente du `0x05` | Émission + accusé | Relecture | Demande → état publié |
|---|---|---|---|---|---|---|
| 2 s | v0.4 | 42 / 1 200 | 1 125 / 2 000 | 70 | 2 045 | 3 282 / 5 290 |
| 2 s | v0.4.1 | 0 / 65 | 1 019 / 2 000 | 70 | 2 045 | **3 134 / 4 155** |
| 3 s | v0.4 | 267 / 3 025 | 1 574 / 2 975 | 70 | 3 045 | 4 956 / 9 115 |
| 3 s | v0.4.1 | 0 / 55 | 1 195 / 2 975 | 70 | 3 045 | **4 311 / 6 145** |
| 4 s | v0.4 | 766 / 4 035 | 2 473 / 3 975 | 70 | 4 045 | 7 354 / 12 125 |
| 4 s | v0.4.1 | 0 / 45 | 1 604 / 3 975 | 70 | 4 045 | **5 719 / 8 135** |

- Le pire cas passe de ~3 à ~2 synchronisations : `état publié ≤ 2 ×
  resynchro + ~0,3 s` (vérifié par le test).
- Le brûleur garde la même cadence qu'en v0.4 (au plus 2 lectures d'écart
  sur 6 h 20). Les programmations sont lues intégralement, sans aucune
  lecture abandonnée après émission.
- Pendant la lecture des 28 blocs de programmation, une commande passe
  devant tous les blocs restants (`sim_v041_command_during_schedule_reading`).
- Pour un programme, la relecture de l'autre circuit ajoute une
  synchronisation **après** la publication de l'état du circuit commandé.

Ces chiffres viennent de la **simulation**. Sur la chaudière, la ligne
`write timing` donne les valeurs réelles. Si `sync wait` et `read-back`
dominent (≈ délai de resynchro de la KW3), il n'y a plus de marge sans
enchaîner les trames, ce qui demanderait une validation matérielle
préalable. Si `line wait` est élevé, ou si l'écart entre la commande et
l'afficheur de la Vitotronic dépasse `state published`, le retard vient
d'ailleurs (régulation, réseau) et doit être analysé.
