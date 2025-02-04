  BitTorrent Protocol

For using MPI logic I created 10 tags that I will work with during the process of sending and receiving messages between clients and tracker.

The first step to start the program is to analyze and manage the work with files. To do this I created 3 structures: for segments (to keep hashes together),
for single files and for client files in general, to keep track of the files we have and the ones we want. So, to analyze the input files stored in in<R>.txt
I used the parsefile function that takes the file name and returns a file constructor with all the necessary features, starting from opening the file and going
to allocating memory for elements and performing checks against errors.To save files in the requested format I created a function that brought together the ordered
list of hash segments.For the first step of client initialization, we read the input file and saved the information from there in the client structure.

In the upload function, used by seeds or peers, a TAG_SEG_REQ is expected from other clients that need segments. This runs until the client receives the final
signal from the tracker. A TAG_SEG_RSP is used, which can signal the success or failure of the process. If we have not received the message that everything is
completed from the tracker, we wait for a request from another client. For this, we use MPI_iprobe to check if there is a message available for the segment request.
If the message is not available, the indicator remains 0. Otherwise, the status will contain information about the message. Next, we receive information about the
filename and index of the segment requested by the client.Then we send the positive or negative message, depending on the result (regardless of whether we have a
valid index or not). Thus, the function checks if there are requests and if the requested segment is available.

We continue with the download thread in which, as the second part of the client initialization,
the client that owns a file sends the tracker the number of segments and the hash of each one.
In this way, the tracker knows which files are in the system, the number of segments,
hashes and who has them in the first place. We wait for an ACK response from the tracker.
Then, for the files that a client requests, we need the number of its segments and the hash of each one. I wait for information about the list of clients.
I search for the file, keep data about it and update the list of seeds/peers.
To simulate the download, I use a localFile for a desired file. I put in it the name of the file, the number of segments and their hashes. Before downloading
the segments, I insert the file as if I had it partially in the file list and when I receive a segment, I copy the segment that I know as if I marked it as received.
Finally, after downloading the segments, I check if I have all the valid segments so that I can save it to localFile. To make it more efficient, I used a distribution
technique in a cyclic manner in the download function. For each segment, we go through the clients that own the file in a cyclic order. Thus, each client receives
requests for segments and the download of a file is not entirely from the same seeds. To see if the download is complete, I check how many segments I have. 
If the number of segments of the file obtained is equal to the actual number of the file that I wanted, it means that everything was completed successfully.
At the end of the download, I can consider the file as owned, inform the tracker and save the file in the requested format. When I have finished downloading
all the files, I send the tag TAG_ALL_DONE to the tracker and close the download thread by updating the field in the client structure for the completion of the
download with 1. For the update mechanism, in the download function after downloading 10 files I ask the tracker for the updated list of seeds/peers and then continue
downloading. In the tracker I also address the case with the tag TAG_WANT_UPDATE, where an updated list of seeds or peers is sent.

As a tracker, I used the Tracker structure, which holds information
about files: the number of segments, the list of clients that have
segments. In the solution, I worked with a single list of clients that I called seeds, which I cycle through, but which I update
when a process announces that it wants to download a certain file and asks for the
tracker's list (TAG_WANT_FILE), the tracker also includes it in the list.
As its initialization, I receive the initial message of each client and add it
later to the list of seeds. The tracker checks whether the current file (with the name fname) already exists
in the file list. Now after we see what message it received, depending on the type of tag,
I follow the instructions for receiving messages from clients. For the situation where we have the completion of a file download, we add the client to the list
and mark it as a seed if it is not already there, we check with a duplicate variable (for duplication). In peer, we ensure the process is stopped, and the tracker
notifies all clients when everything is completed.
