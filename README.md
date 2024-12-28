# APD_2
In functia de upload , folosita de seeds sau peers se
asteapta un TAG_SEG_REQ din partea altor clienti care
au nevoie de segmente. Se returneaza un TAG_SEG_RSP,
care poate sa semnaleze finalizarea cu succes sau nu, a procesului.
In cazul in care nu am primit mesajul ca totul este finalizat de la tracker astept
un request de la alt client . Pentru asta folosesc MPI_iprobe pentru a verifica
daca exista mesaj disponibil pe tagul de cerere de segment de la orice sursa. Daca mesajul nu
este disponibil, flagul ramane 0. In caz contrar in status se va afla informatia
despre mesaj. In continuare primim informatii despre filename si indexul segmentului
cerut de client. Sursa o identificam folosid statusul anterior.In continuare 
cautam printre fisierele detinute de client, comparam numele acestora cu
numele fisierului primit anterior. Apoi trimitem mesajul pozitiv sau negativ, in functie de
rezultat(dac aavem sau nu un index valid).

Continuam cu threadul de download in care clientul care detine un fisier ii 
trimite trackerului numarul de segmente si hash-ul fiecaruia. Apoi pentru
fisierele pe care le cere un client de la tracker avem nevoie de numarul de 
segmente ale acestuia si hashul fiecaruia. Caut fisierul, pastrez date despre
el si actaulizez lista de seeds.Pentru eficientizare am folosit in functia de descarcare o tehnica de distribui>
in mod ciclic. Pentru fiecare segment parcurgem in ordine ciclica clientii care
detin fisierul complet. Astfel fiecare seed primeste cereri pentru segmente in
mod ciclic iar descarcarea unui fisier nu se face in totalitate de la
acelasi seed.Pentru a vedea daca descarcarea e finalizata 
verific cate segmente am. Daca numarul de segmnte al fisierului obtinut
este egal cu cel real al fisierului pe care il voiam inseamna ca 
totul s-a finalizat cu succes.  La finalul descarcarii pot considera fisierul
ca detinut , informez trackerul si salvez in formatul cerut

In functia de tracker m am folosit de structura Tracker, ce retine informatii
despre fisiere: numele , numarul de segmnete, lista clientilor care au
segmente din fisierul respectiv. In urma primirii mesajului de la client am
tratat cazurile specificate in enunt.
