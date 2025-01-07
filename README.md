  Margheanu Cristina-Andreea 333CA

  Pentru utilizarea logicii MPI am creat 10 tag-uri cu care voi lucra în timpul
procesului de trimitere și primire a mesajelor intre clienti si tracker.

  Primul pas pentru pornirea programului constă în analizarea și gestionarea 
lucrului cu fisierele. Pentru a face acest lucru am creat 3 structuri: pentru
segmente (pentru a păstra hashurile împreună), pentru fișiere singulare și pentru
fișierele clientului în general, pentru a urmări fisierele pe care le avem si pe 
cele pe care le dorim. Deci, pentru a analiza fișierele de intrare stocate in in<R>.txt
am folosit funcția parsefile care preia numele fișierului și returnează un constructor de
fișier cu toate caracteristicile necesare, pornind de la deschiderea fișierului și mergând
până la alocare memorie pentru elemente și efectuarea verificărilor împotriva erorilor.
Pentru a salva fișiere în formatul solicitat am creat o funcție care a reunit
lista ordonată a segmentelor de hashuri. 
  Pentru primul pas de inițializare a clientului am citit fișierul de intrare și am salvat
informatiile de acolo in structura clientului.

  În funcția de upload, utilizată de seeds sau peers, este așteptat un TAG_SEG_REQ de la alti
clienti care au nevoie de segmente. Aceasta ruleaza pana cand clientul primeste semnalul final
de la tracker. Este folosit un TAG_SEG_RSP, care poate semnala succesul sau finalizarea
nereușită a procesului. Dacă nu am primit mesajul că totul este finalizat din tracker, așteptăm
o solicitare de la alt client. Pentru aceasta, folosim MPI_iprobe pentru a verifica dacă există
un mesaj disponibil la cererea de segment. Dacă mesajul nu este disponibil,
indicatorul rămâne 0. În caz contrar, statusul va conține informații despre mesaj. În continuare, 
primim informații despre numele de fișier și indexul segmentului solicitat de client.
Apoi trimitem mesajul pozitiv sau negativ, în funcție de rezultat (indiferent dacă avem
sau nu un index valid). Astfel, functia verifica daca exista cereri si daca segmentul cerut este 
disponibil.

  Continuăm cu threadul de descărcare în care, ca a doua parte a inițializării clientului,
clientul care deține un fișier trimite tracker-ului numărul de segmente și hash-ul fiecăruia.
În acest fel, tracker-ul știe care sunt fișierele din sistem, numărul de segmente,
hashes și cine le are la început. Așteptăm un răspuns ACK de la tracker.
Apoi, pentru fișierele pe care un client le solicită avem nevoie de numărul de
segmente ale acestuia și de hash-ul fiecăruia. Astept informatii despre lista clientilro.
Caut fișierul, păstrez datele despre acesta și actualizați lista de seeds/peers. 
Pentru simularea descarcarii folosesc un localFile  pentru un fisier dorit. Pun
în el numele fisierului, numarul de segmente si hashurile lor. Inainte de descarcarea
segmentelor introduc fisierul ca si cum l-as avea partial il lista fisierelor si cand
primesc un segment copiez segmentul pe care il stiu ca si cum as marca ca l-am primit.
La final după descarcarea segmentelor, verific daca am toate segementele valide deci
localFile pot sa-l salvez. Pentru a-l face mai eficient, am folosit o tehnică de distribuire
într-o manieră ciclică în funcția de descărcare. Pentru fiecare segment, parcurgem clienții
care dețin fișierul într-o ordine ciclică. Astfel, fiecare client primește cereri
de segmente  și descărcarea unui fișier nu se face în întregime din 
aceleasi seeds .Pentru a vedea dacă descărcarea este finalizată, verific cate segmente am.
Dacă numărul de segmente ale fișierului obținut este egal cu cel real al fisierul pe care
l-am dorit, inseamna ca totul a fost finalizat cu succes. 
La sfârșitul descărcării, pot considerați fișierul ca deținut, informez trackerul și salvez
fisierul în formatul cerut. Cand am terminat de descarcat toate fisierele trimitem tagul
TAG_ALL_DONE trackerului si inchidem threadul de download
actualizand campul din structura client de finalizare a downloadarii cu 1.
  Pentru mecanismul de actualizare, in functia de download dupa ce descarc 10
fisiere cer trackerului lista actualizata de seeds/peers apoi continui cu descarcarea.
In tracker abordez si cazul cu tag-ul TAG_WANT_UPDATE, unde este trimisa o lista actualizata de
seeds sau peers.

  Ca tracker, am folosit structura Tracker, care deține informații
despre fisiere: numarul de segmente, lista clientilor care au
segmente. In cadrul rezolvarii am lucrat cu o singura lista de 
clienti pe care am numit-o seeds, pe care o parcurg ciclic, dar pe care o actualizez 
atunci cand un proces anunta ca vrea sa downloadeze un anumit fisier si cere lista 
trackerului(TAG_WANT_FILE), trackerul il include si pe el in lista. 
Ca initializare a acestuia primesc mesajul initial al fiecarui client si il adaug
ulterior la lista de seeds. Trackerul verifica daca fisierul curent (cu numele fname) exista
deja in lista de fisiere. Acum dupa ce vedem ce mesaj a primit, in functie de tipul tagului,
urmez indicatiile de primire a mesajelor de la clienti. Pentru situatia in care avem finalizarea
descarcarii unui fisier adaugam clientul in lista si il marcam ca si seed daca nu este deja acolo,
verificam cu o variabila duplicate(pentru duplicare).
  In peer asigur oprirea procesului , iar trackerul notifica toti clientii cand totul este finalizat.
