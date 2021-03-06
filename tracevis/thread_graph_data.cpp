/*
Copyright 2016 Nia Catlin

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

/*
Monsterous class that handles the bulk of graph management
*/

#include "stdafx.h"
#include "thread_graph_data.h"
#include "thread_trace_reader.h"
#include "rendering.h"
#include "serialise.h"


bool thread_graph_data::isGraphBusy() 
{
	bool busy = (WaitForSingleObject(graphwritingMutex, 0) == WAIT_TIMEOUT);
	if (!busy)
		ReleaseMutex(graphwritingMutex);
	return busy;
}

void thread_graph_data::insert_edge_between_BBs(INSLIST *source, INSLIST *target)
{
	INS_DATA *sourceIns = source->back();
	INS_DATA *targetIns = target->front();

	unsigned int sourceNodeIdx = sourceIns->threadvertIdx.at(tid);
	unsigned int targNodeIdx = targetIns->threadvertIdx.at(tid);

	NODEPAIR edgeNodes = make_pair(sourceNodeIdx, targNodeIdx);

	if (edgeDict.count(edgeNodes)) return;

	node_data *sourceNode = safe_get_node(sourceNodeIdx);
	node_data *targNode = safe_get_node(targNodeIdx);

	edge_data newEdge;
	
	if (targNode->external)
		newEdge.edgeClass = ILIB;
	else if (sourceNode->ins->itype = OPCALL)
		newEdge.edgeClass = ICALL;
	else if (sourceNode->ins->itype = OPRET)
		newEdge.edgeClass = IRET;
	else
		newEdge.edgeClass = IOLD;

	add_edge(newEdge, sourceNode, targNode);

}

void thread_graph_data::setGraphBusy(bool set) 
{
	if (set) {
		DWORD res = WaitForSingleObject(graphwritingMutex, 1000);
		if (res == WAIT_TIMEOUT)
			cerr << "[rgat]Timeout waiting for release of graph " << tid << endl;
		assert(res != WAIT_TIMEOUT);
	}
	else ReleaseMutex(graphwritingMutex);
}


//add new extern calls to log
unsigned int thread_graph_data::fill_extern_log(ALLEGRO_TEXTLOG *textlog, unsigned int logSize)
{
	vector <string>::iterator logIt = loggedCalls.begin();
	advance(logIt, logSize);
	while (logIt != loggedCalls.end())
	{
		al_append_native_text_log(textlog, logIt->c_str());
		logSize++;
		logIt++;
	}
	return logSize;
}

//returns combined count of read+processing trace buffers
unsigned long thread_graph_data::get_backlog_total()
{
	if (!this->trace_reader) return 0;
	thread_trace_reader *reader = (thread_trace_reader *)trace_reader;
	pair <unsigned long, unsigned long> sizePair;
	reader->getBufsState(&sizePair);
	return sizePair.first + sizePair.second;
}

//display live or animated graph with active areas on faded areas
void thread_graph_data::display_active(bool showNodes, bool showEdges)
{
	GRAPH_DISPLAY_DATA *nodesdata = get_activenodes();
	GRAPH_DISPLAY_DATA *linedata = get_activelines();

	//reload buffers if needed and not being written
	if (needVBOReload_active && !isGraphBusy())
	{
		setGraphBusy(true);
		load_VBO(VBO_NODE_POS, activeVBOs, mainnodesdata->pos_size(), mainnodesdata->readonly_pos());
		load_VBO(VBO_NODE_COL, activeVBOs, animnodesdata->col_size(), animnodesdata->readonly_col());

		GLfloat *buf = mainlinedata->readonly_pos();
		if (!buf) { setGraphBusy(false); return; }
		int posbufsize = mainlinedata->get_numVerts() * POSELEMS * sizeof(GLfloat);
		load_VBO(VBO_LINE_POS, activeVBOs, posbufsize, buf);

		buf = animlinedata->readonly_col();
		if (!buf) { setGraphBusy(false); return; }
		int linebufsize = animlinedata->get_numVerts() * COLELEMS * sizeof(GLfloat);
		load_VBO(VBO_LINE_COL, activeVBOs, linebufsize, buf);

		needVBOReload_active = false;
		setGraphBusy(false);
	}

	if (showNodes)
		array_render_points(VBO_NODE_POS, VBO_NODE_COL, activeVBOs, nodesdata->get_numVerts());

	if (showEdges)
		array_render_lines(VBO_LINE_POS, VBO_LINE_COL, activeVBOs, linedata->get_numVerts());
}

//display graph with everything bright and viewable
void thread_graph_data::display_static(bool showNodes, bool showEdges)
{
	if (needVBOReload_main && !isGraphBusy())
	{
		setGraphBusy(true);
		//lock for reading?
		loadVBOs(graphVBOs, mainnodesdata, mainlinedata);
		needVBOReload_main = false;
		setGraphBusy(false);
	}
	
	if (showNodes)
		array_render_points(VBO_NODE_POS, VBO_NODE_COL, graphVBOs, mainnodesdata->get_numVerts());

	if (showEdges)
		array_render_lines(VBO_LINE_POS, VBO_LINE_COL, graphVBOs, mainlinedata->get_numVerts());
}

//create faded edge version of graph for use in animations
void thread_graph_data::extend_faded_edges()
{

	vector<GLfloat> *animecol = animlinedata->acquire_col_write();
	vector<GLfloat> *mainecol = mainlinedata->acquire_col_write();
	unsigned int drawnVerts = mainlinedata->get_numVerts();
	unsigned int animatedVerts = animlinedata->get_numVerts();

	assert(drawnVerts >= animatedVerts);
	int pendingVerts = drawnVerts - animatedVerts;
	if (!pendingVerts) return;

	//copy the colours over
	unsigned int fadedIndex = animlinedata->get_numVerts() *COLELEMS;
	vector<float>::iterator mainEIt= mainecol->begin();
	advance(mainEIt, fadedIndex);
	animecol->insert(animecol->end(), mainEIt, mainecol->end());
	mainlinedata->release_col_write();

	//fade new colours alpha
	unsigned int index2 = (animlinedata->get_numVerts() *COLELEMS);
	unsigned int end = drawnVerts*COLELEMS;
	for (; index2 < end; index2 += COLELEMS)
		animecol->at(index2 + AOFF) = 0.01; //TODO: config file entry for anim inactive

	animlinedata->set_numVerts(drawnVerts);
	animlinedata->release_col_write();
}

//create edges in opengl buffers
void thread_graph_data::render_new_edges(bool doResize, map<int, ALLEGRO_COLOR> *lineColoursArr)
{
	GRAPH_DISPLAY_DATA *lines = get_mainlines();
	EDGELIST::iterator edgeIt;

	getEdgeReadLock();
	if (doResize)
	{
		reset_mainlines();
		lines = get_mainlines();
		edgeIt = edgeList.begin();
	}
	else
	{
		edgeIt = edgeList.begin();
		std::advance(edgeIt, lines->get_renderedEdges());
	}

	if (edgeIt != edgeList.end())
		needVBOReload_main = true;

	for (; edgeIt != edgeList.end(); ++edgeIt)
	{
		render_edge(*edgeIt, lines, lineColoursArr);
		extend_faded_edges();
		lines->inc_edgesRendered();
	}
	dropEdgeReadLock();
}

/*
//given a sequence id, get the last instruction in the block it refers to
INS_DATA* thread_graph_data::get_last_instruction(unsigned long sequenceId)
{
	obtainMutex(animationListsMutex, 1016);
	pair<MEM_ADDRESS, int> targBlock_Size = bbsequence.at(sequenceId);
	BLOCK_IDENTIFIER blockID = mutationSequence.at(sequenceId);
	dropMutex(animationListsMutex);

	MEM_ADDRESS insAddr = targBlock_Size.first;
	int instructionIndex = targBlock_Size.second-1;
	
	return getDisassemblyBlock(insAddr, blockID, piddata, &terminationFlag)->at(instructionIndex);
}*/

/*
//externs not included in sequence data, have to check if each block called one
void thread_graph_data::display_extern_text(NODEINDEX node, unsigned int callIndex)
{
	getNodeReadLock();
	node_data *n = locked_get_node(targetExternIdx);
	vector<ARGLIST> *funcArgs = &n->funcargs;
	ex.nodeIdx = n->index;
	dropNodeReadLock();
	ex.drawFloating = updateArgs;


	
	obtainMutex(funcQueueMutex, 1018);
	string funcArgString;
	if (updateArgs)
	{
		if (!n->funcargs.empty())
			if (callsSoFar < funcArgs->size())
				ex.argList = funcArgs->at(callsSoFar);
			else
				ex.argList = *funcArgs->rbegin();
	}

	floatingExternsQueue.push(ex);
	dropMutex(funcQueueMutex);

}
*/

string thread_graph_data::get_node_sym(NODEINDEX idx, PROCESS_DATA* piddata)
{
	node_data *n = safe_get_node(idx);
	string sym;

	if (!piddata->get_sym(n->nodeMod, n->address, &sym))
	{
	
		string modPath;
		if (!piddata->get_modpath(n->nodeMod, &modPath))
			cerr << "[rgat]WARNING: mod " << n->nodeMod << " expected but not found" << endl;

		stringstream nosym;
		nosym << basename(modPath) << ":0x" << std::hex << n->address;
		return nosym.str();
	}

	return sym;
}

void thread_graph_data::emptyArgQueue()
{
	obtainMutex(funcQueueMutex, 1019);
	while (!floatingExternsQueue.empty()) floatingExternsQueue.pop();
	dropMutex(funcQueueMutex);
}

/*
bool thread_graph_data::decrease_sequence()
{
	return true; //unimplemented, better things to do at the moment
}

bool thread_graph_data::advance_sequence(bool skipLoop = false)
{
	if (sequenceIndex + 1 >= bbsequence.size()) return false;

	animInstructionIndex += bbsequence.at(sequenceIndex).second;
	//if not looping
	if (!loopStateList.at(sequenceIndex).first)
	{
		brighten_externs(++sequenceIndex, true);
		return true;
	}

	//first we update loop progress

	//just started loop
	if (!animLoopStartIdx)
	{
		targetIterations = loopStateList.at(sequenceIndex).second;
		animLoopIndex = 0;
		animLoopStartIdx = sequenceIndex;
		loopIteration = 1;
		animLoopProgress.push_back(loopIteration);
	}
	//block of first iteration of loop
	else if (animLoopIndex > animLoopProgress.size() - 1)
	{
		loopIteration = 1;
		animLoopProgress.push_back(loopIteration);
	}
	else
	{
		loopIteration = animLoopProgress.at(animLoopIndex) + 1;
		animLoopProgress.at(animLoopIndex) = loopIteration;
	}

	brighten_externs(sequenceIndex, true);

	//now set where to go next
	//last iteration of loop
	if (skipLoop || (loopStateList.at(sequenceIndex).second == animLoopProgress.at(animLoopCounter)))
	{
		//last block of loop
		if ((animLoopIndex >= animLoopProgress.size() - 1) || skipLoop)
		{
			++loopsPlayed;
			animLoopProgress.clear();
			animLoopStartIdx = 0;
			animLoopIndex = 0;
		}
		else
			++animLoopIndex;
		
		if (sequenceIndex + 1 >= bbsequence.size()) return false;
		++sequenceIndex;

		if (skipLoop)
			while (loopStateList.at(sequenceIndex).first)
				++sequenceIndex;
	}

	//last block of loop but not last iteration
	else if (loopStateList.at(sequenceIndex).first != loopStateList.at(sequenceIndex + 1).first)
	{

		sequenceIndex = animLoopStartIdx;
		animLoopIndex = 0;
		
	}
	else //inside loop
	{
		if (sequenceIndex + 1 >= bbsequence.size()) return false;
		++sequenceIndex;
		++animLoopIndex;
	}
	return true;
}

void thread_graph_data::performStep(int stepSize, bool skipLoop = false)
{

	if (stepSize > 0)
	{
		for (int i = 0; i < stepSize; ++i)
			if (!advance_sequence(skipLoop)) break;

	}
	else if (stepSize < 0)
	{
		stepSize *= -1;
		for (int i = 0; i < stepSize; ++i)
			decrease_sequence();
	}

	set_active_node(derive_anim_node());
}

unsigned int thread_graph_data::updateAnimation(unsigned int updateSize, bool animationMode, bool skipLoop = false)
{
	if (nodeList.empty()) return ANIMATION_ENDED;

	performStep(updateSize, skipLoop);
	if (!animationMode) return 0;

	bool animation_end = false;

	if (sequenceIndex >= (bbsequence.size() - 1))
		return ANIMATION_ENDED;

	return 0;
}

void thread_graph_data::darken_animation(float alphaDelta)
{

	if (!animlinedata->get_numVerts()) return;
	GLfloat *ecol = &animlinedata->acquire_col()->at(0);

	map<NODEPAIR, edge_data *>::iterator activeEdgeIt = activeEdgeMap.begin();
	bool update = false;

	if (activeEdgeIt != activeEdgeMap.end()) 
		update = true;
	while (activeEdgeIt != activeEdgeMap.end())
	{
		edge_data *e = activeEdgeIt->second;
		if (!e->vertSize) { 
			++activeEdgeIt;
			continue; 
		}

		unsigned long edgeStart = e->arraypos;
		float edgeAlpha;
		float lowestAlpha = 0;

		for (unsigned int i = 0; i < e->vertSize; ++i)
		{
			const unsigned int colBufIndex = edgeStart + i*COLELEMS + AOFF;
			if (colBufIndex >= animlinedata->col_buf_capacity_floats())
			{
				//sometimes happens if terminated at a bad time. will fix itself
				cerr << "[rgat]Warning: colbufIndex > capacity" << endl;
				animlinedata->release_col();
				return;
			}
			edgeAlpha = ecol[colBufIndex];

			//TODO: problems here!
			//0.05 stored as 0.05000000002
			//0.06 stored as 0.59999999999997 [not the real number of 0's]
			edgeAlpha = fmax(ANIM_INACTIVE_EDGE_ALPHA, edgeAlpha - alphaDelta);
			ecol[colBufIndex] = edgeAlpha;
			lowestAlpha = fmax(lowestAlpha, edgeAlpha);
		}	

		//workaround to problem, fails with 0.05, works with 0.06
		if (lowestAlpha <= ANIM_INACTIVE_EDGE_ALPHA)
			activeEdgeIt = activeEdgeMap.erase(activeEdgeIt);
		else
			++activeEdgeIt;
	}
	animlinedata->release_col();
	GLfloat *ncol = &animnodesdata->acquire_col()->at(0);
	int colBufSize = animnodesdata->col_buf_capacity_floats();

	map<unsigned int, bool>::iterator activeNodeIt = activeNodeMap.begin();
	if (activeNodeIt != activeNodeMap.end()) 
		update = true;
	while (activeNodeIt != activeNodeMap.end())
	{
		unsigned int nodeIndex = activeNodeIt->first;
		node_data *n = get_node(nodeIndex);
		int colBufIndex = (nodeIndex * COLELEMS) + AOFF;
		if (colBufIndex >= colBufSize) break;
		float currentAlpha = ncol[colBufIndex];
		currentAlpha = fmax(ANIM_INACTIVE_NODE_ALPHA, currentAlpha - alphaDelta);
		ncol[colBufIndex] = currentAlpha;

		if (currentAlpha == ANIM_INACTIVE_NODE_ALPHA)
			activeNodeIt = activeNodeMap.erase(activeNodeIt);
		else
			++activeNodeIt;
	}

	animnodesdata->release_col();
	if (update) needVBOReload_active = true;
}
*/
void thread_graph_data::reset_animation()
{
	last_anim_start = 0;
	last_anim_stop = 0;
	animInstructionIndex = 0;
	newanim = true;

	animationIndex = 0;
	blockInstruction = 0;
	if (!nodeList.empty())
	{
		set_active_node(0);
		darken_fading(1);
	}
	firstAnimatedBB = 0;
	lastAnimatedBB = 0;
	newAnimEdgeTimes.clear();
	newAnimNodeTimes.clear();
	darken_fading(1);
	activeAnimEdgeTimes.clear();
	activeAnimNodeTimes.clear();
	//loopsPlayed = 0;
	//loopIteration = 0;
	//targetIterations = 0;
	callCounter.clear();
}

bool thread_graph_data::fill_block_vertlist(MEM_ADDRESS blockAddr, BLOCK_IDENTIFIER blockID, vector <NODEINDEX> *vertlist)
{
	INSLIST * block = getDisassemblyBlock(blockAddr, blockID, piddata, &terminationFlag);
	if (!block)
	{
		piddata->getExternlistReadLock();
		EDGELIST callvs = piddata->externdict.at(blockAddr)->thread_callers.at(tid);
		piddata->dropExternlistReadLock();

		EDGELIST::iterator callvsIt = callvs.begin();
		for (; callvsIt != callvs.end(); ++callvsIt) //run through each function with a new arg
		{
			if (callvsIt->first == lastAnimatedNode)
				vertlist->push_back(callvsIt->second);
		}
		return true;
	}

	INSLIST::iterator blockIt = block->begin();
	for (; blockIt != block->end(); ++blockIt)
	{
		INS_DATA* activeIns = *blockIt;
		unordered_map<PID_TID, NODEINDEX>::iterator vertIt = activeIns->threadvertIdx.find(tid);
		if (vertIt == activeIns->threadvertIdx.end())
			return false;
		vertlist->push_back(vertIt->second);
	}
	return true;
}

void thread_graph_data::remove_unchained_from_animation()
{
	map <NODEINDEX, int>::iterator newNodeIt = newAnimNodeTimes.begin();
	for (; newNodeIt != newAnimNodeTimes.end(); ++newNodeIt)
		if (newNodeIt->second == KEEP_BRIGHT)
		{
			newNodeIt = newAnimNodeTimes.erase(newNodeIt);
			if (newNodeIt == newAnimNodeTimes.end()) break;
		}

	map <NODEPAIR, int>::iterator newEdgeIt = newAnimEdgeTimes.begin();
	for (; newEdgeIt != newAnimEdgeTimes.end(); ++newEdgeIt)
		if (newEdgeIt->second == KEEP_BRIGHT)
		{
			newEdgeIt = newAnimEdgeTimes.erase(newEdgeIt);
			if (newEdgeIt == newAnimEdgeTimes.end()) break;
		}

	map <unsigned int, int>::iterator nodeIt = activeAnimNodeTimes.begin();
	for (; nodeIt != activeAnimNodeTimes.end(); ++nodeIt)
		if (nodeIt->second == KEEP_BRIGHT)
			nodeIt->second = 0;

	map <NODEINDEX, EXTTEXT>::iterator activeExternIt = activeExternTimes.begin();
	for (; activeExternIt != activeExternTimes.end(); ++activeExternIt)
		if (activeExternIt->second.framesRemaining == KEEP_BRIGHT)
			activeExternIt->second.framesRemaining = (int)(EXTERN_LIFETIME_FRAMES / 2);

	map <NODEPAIR, int>::iterator edgeIt = activeAnimEdgeTimes.begin();
	for (; edgeIt != activeAnimEdgeTimes.end(); ++edgeIt)
		if (edgeIt->second == KEEP_BRIGHT)
			edgeIt->second = 0;
}

void thread_graph_data::process_live_animation_updates()
{
	if (animUpdates.empty()) return;

	bool activeUnchained = false;
	while (!animUpdates.empty())
	{
		obtainMutex(animationListsMutex, 6210);
		ANIMATIONENTRY entry = animUpdates.front();
		dropMutex(animationListsMutex);

		NODEINDEX backupLastAnimNode = lastAnimatedNode;

		if (entry.entryType == ANIM_UNCHAINED_RESULTS)
		{
			remove_unchained_from_animation();

			obtainMutex(animationListsMutex, 6211);
			savedAnimationData.push_back(entry);
			animUpdates.pop();
			dropMutex(animationListsMutex);
			continue;
		}

		if (entry.entryType == ANIM_UNCHAINED_DONE)
		{
			currentUnchainedBlocks.clear();
			NODEINDEX firstChainedNode = getDisassemblyBlock(entry.blockAddr, entry.blockID, piddata, &terminationFlag)->back()->threadvertIdx.at(tid);
			lastAnimatedNode = firstChainedNode;

			obtainMutex(animationListsMutex, 6211);
			savedAnimationData.push_back(entry);
			animUpdates.pop();
			dropMutex(animationListsMutex);
			continue;
		}


		int brightTime;
		if (entry.entryType == ANIM_UNCHAINED)
		{
			currentUnchainedBlocks.push_back(entry);
			brightTime = KEEP_BRIGHT;
			activeUnchained = true;
		}
		else
			brightTime = 0;

		vector <NODEINDEX> nodeIDList;

		//break if block not rendered yet
		
		if (!fill_block_vertlist(entry.blockAddr, entry.blockID, &nodeIDList))
		{
			if (entry.entryType != ANIM_EXEC_EXCEPTION) break;
			//expect to get an incomplete block with exception
			if (nodeIDList.size() < entry.count) break;
		}

		//add all the nodes+edges in the block to the brightening list
		int instructionCount = 0;
		vector <NODEINDEX>::iterator nodeIt = nodeIDList.begin();
		for (; nodeIt != nodeIDList.end(); ++nodeIt)
		{
			NODEINDEX nodeIdx = *nodeIt;
			newAnimNodeTimes[nodeIdx] = brightTime;

			if (safe_get_node(nodeIdx)->external)
			{
				if (brightTime == KEEP_BRIGHT)
					newExternTimes[nodeIdx] = KEEP_BRIGHT;
				else
					newExternTimes[nodeIdx] = EXTERN_LIFETIME_FRAMES;
			}
			if ((animInstructionIndex != 0) && //cant draw edge to first node in animation
				//edge to unchained area is not part of unchained area
				!(entry.entryType == ANIM_UNCHAINED && nodeIt == nodeIDList.begin())) 
			{
				NODEPAIR edge = make_pair(lastAnimatedNode, nodeIdx);
				if (!edge_exists(edge, 0))
				{
					cerr << "[rgat]ERROR: Tried to animate non-existing edge!" << endl;
					assert(0);
				}
				newAnimEdgeTimes[edge] = brightTime;
			}
			lastAnimatedNode = nodeIdx;
			
			++instructionCount;
			if ((entry.entryType == ANIM_EXEC_EXCEPTION) && (instructionCount == (entry.count+1))) break;
		}

		//also add brighten edge to next unchained block
		if (entry.entryType == ANIM_UNCHAINED)
		{
			NODEINDEX nextNode;
			NODEPAIR linkingPair;
			if (piddata->externdict.count(entry.targetAddr))
			{
				EDGELIST callers = piddata->externdict.at(entry.targetAddr)->thread_callers.at(tid);
				EDGELIST::iterator callIt = callers.begin();
				for (; callIt != callers.end(); ++callIt)
					if (callIt->first == lastAnimatedNode)
					{
						nextNode = callIt->second;
						linkingPair = make_pair(lastAnimatedNode, nextNode);
						break;
					}
				assert(callIt != callers.end());
			}
			else
			{
				INSLIST* nextBlock = getDisassemblyBlock(entry.targetAddr, entry.targetID, piddata, &terminationFlag);
				INS_DATA* nextIns = nextBlock->front();
				nextNode = nextIns->threadvertIdx.at(tid);
				linkingPair = make_pair(lastAnimatedNode, nextNode);
			}
			

			if (!edge_exists(linkingPair, 0)) 
				break;

			newAnimEdgeTimes[linkingPair] = brightTime;
		}

		animInstructionIndex += instructionCount;

		obtainMutex(animationListsMutex, 6212);
		savedAnimationData.push_back(entry);
		animUpdates.pop();
		dropMutex(animationListsMutex);	
	}
}


#define ASSUME_INS_PER_BLOCK 10
//tries to make animation pause for long enough to represent heavy cpu usage but
//not too long to make it irritating
//if program is 1m instructions and takes 10s to execute then a 50k block should wait for ~.5s
unsigned long thread_graph_data::calculate_wait_frames(unsigned int stepSize, unsigned long blockInstructions)
{
	//assume 10 instructiosn per step/frame
	unsigned long frames = (totalInstructions/ ASSUME_INS_PER_BLOCK) / stepSize;

	float proportion = (float)blockInstructions / totalInstructions;
	unsigned long waitFrames = proportion*frames;
	return waitFrames;
}

int thread_graph_data::process_replay_animation_updates(int stepSize)
{
	unsigned long targetAnimIndex = animationIndex + stepSize;
	if (targetAnimIndex >= savedAnimationData.size())
		targetAnimIndex = savedAnimationData.size() - 1;

	for (; animationIndex < targetAnimIndex; ++animationIndex)
	{
		ANIMATIONENTRY entry = savedAnimationData.at(animationIndex);

		//unchained area finished, stop highlighting it
		if (entry.entryType == ANIM_UNCHAINED_RESULTS)
		{
			
			INSLIST *block = getDisassemblyBlock(entry.blockAddr, entry.blockID, piddata, &terminationFlag);

			unchainedWaitFrames += calculate_wait_frames(stepSize, entry.count*block->size());
			
			if (unchainedWaitFrames > maxWaitFrames) 
				unchainedWaitFrames = maxWaitFrames;
			continue;
		}

		//all consecutive unchained areas finished, wait until animation paused appropriate frames
		if (entry.entryType == ANIM_UNCHAINED_DONE)
		{
			if (unchainedWaitFrames > 0)
			{
				--unchainedWaitFrames;
				break;
			}

			remove_unchained_from_animation();
			currentUnchainedBlocks.clear();
			NODEINDEX firstChainedNode = getDisassemblyBlock(entry.blockAddr, entry.blockID, 
				piddata, &terminationFlag)->back()->threadvertIdx.at(tid);
			lastAnimatedNode = firstChainedNode;
			continue;
		}

		int brightTime;
		if (entry.entryType == ANIM_UNCHAINED)
		{
			currentUnchainedBlocks.push_back(entry);
			brightTime = KEEP_BRIGHT;
		}
		else
			brightTime = 20;

		vector <NODEINDEX> nodeIDList;

		if (!fill_block_vertlist(entry.blockAddr, entry.blockID, &nodeIDList))
		{
			assert(entry.entryType == ANIM_EXEC_EXCEPTION);
		}

		//add all the nodes+edges in the block to the brightening list
		int instructionCount = 0;
		vector <NODEINDEX>::iterator nodeIt = nodeIDList.begin();
		for (; nodeIt != nodeIDList.end(); ++nodeIt)
		{
			NODEINDEX nodeIdx = *nodeIt;
			newAnimNodeTimes[nodeIdx] = brightTime;

			if (safe_get_node(nodeIdx)->external)
			{
				if (brightTime == KEEP_BRIGHT)
					newExternTimes[nodeIdx] = KEEP_BRIGHT;
				else
					newExternTimes[nodeIdx] = EXTERN_LIFETIME_FRAMES;
			}
			if ((animInstructionIndex != 0) && //cant draw edge to first node in animation
											   //edge to unchained area is not part of unchained area
				!(entry.entryType == ANIM_UNCHAINED && nodeIt == nodeIDList.begin()))
			{
				NODEPAIR edge = make_pair(lastAnimatedNode, nodeIdx);
				assert(edge_exists(edge, 0));
				newAnimEdgeTimes[edge] = brightTime;
			}
			lastAnimatedNode = nodeIdx;

			++instructionCount;
			if ((entry.entryType == ANIM_EXEC_EXCEPTION) && (instructionCount == (entry.count + 1))) break;
		}

		//also add brighten edge to next unchained block
		if (entry.entryType == ANIM_UNCHAINED)
		{
			NODEINDEX nextNode;
			NODEPAIR linkingPair;
			if (piddata->externdict.count(entry.targetAddr))
			{
				EDGELIST callers = piddata->externdict.at(entry.targetAddr)->thread_callers.at(tid);
				EDGELIST::iterator callIt = callers.begin();
				for (; callIt != callers.end(); ++callIt)
					if (callIt->first == lastAnimatedNode)
					{
						nextNode = callIt->second;
						linkingPair = make_pair(lastAnimatedNode, nextNode);
						break;
					}
				assert(callIt != callers.end());
			}
			else
			{
				INSLIST* nextBlock = getDisassemblyBlock(entry.targetAddr, entry.targetID, piddata, &terminationFlag);
				INS_DATA* nextIns = nextBlock->front();
				nextNode = nextIns->threadvertIdx.at(tid);
				linkingPair = make_pair(lastAnimatedNode, nextNode);
			}


			assert(edge_exists(linkingPair, 0));

			newAnimEdgeTimes[linkingPair] = brightTime;
		}

		animInstructionIndex += instructionCount;
	}

	set_active_node(lastAnimatedNode);

	if (animationIndex >= savedAnimationData.size() - 1)
		return ANIMATION_ENDED;
	else return 0;

}

//not part of maintain_active because al_draw_text has to be called from the opengl context holding thread
void thread_graph_data::draw_externTexts(ALLEGRO_FONT *font, bool nearOnly, int left, int right, int height, PROJECTDATA *pd)
{
	DCOORD nodepos;
	map <NODEINDEX, EXTTEXT>::iterator activeExternIt = activeExternTimes.begin();
	for (; activeExternIt != activeExternTimes.end(); ++activeExternIt)
	{
		EXTTEXT *extxt = &activeExternIt->second;

		if (extxt->framesRemaining != KEEP_BRIGHT)
		{
			extxt->yOffset += EXTERN_FLOAT_RATE;

			if (extxt->framesRemaining-- == 0)
			{
				activeExternIt = activeExternTimes.erase(activeExternIt);
				if (activeExternIt == activeExternTimes.end())
					break;
				else
					continue;
			}
		}
		getNodeReadLock();
		node_data *n = unsafe_get_node(activeExternIt->first);
		if (nearOnly && !a_coord_on_screen(n->vcoord.a, left, right, m_scalefactors->HEDGESEP))
		{
			dropNodeReadLock(); 
			continue;
		}

		if (!n->get_screen_pos(mainnodesdata, pd, &nodepos)) 
			{dropNodeReadLock(); continue;}

		dropNodeReadLock();

		al_draw_text(font, al_col_green, nodepos.x, height - nodepos.y - extxt->yOffset, 
			0, extxt->displayString.c_str());
	}
}

void thread_graph_data::maintain_active()
{
	map<unsigned int, int>::iterator nodeAPosTimeIt = activeAnimNodeTimes.begin();
	GLfloat *ncol = &animnodesdata->acquire_col_write()->at(0);
	for (; nodeAPosTimeIt != activeAnimNodeTimes.end(); ++nodeAPosTimeIt)
	{
		int brightTime = nodeAPosTimeIt->second;
		if (brightTime == KEEP_BRIGHT) 
		{ 
			ncol[nodeAPosTimeIt->first] = getPulseAlpha();
			continue; 
		}

		if (--nodeAPosTimeIt->second <= 0)
		{
			fadingAnimNodes.insert(nodeAPosTimeIt->first);
			nodeAPosTimeIt = activeAnimNodeTimes.erase(nodeAPosTimeIt);
			if (nodeAPosTimeIt == activeAnimNodeTimes.end()) break;
		}
	}
	animnodesdata->release_col_write();

	map<NODEPAIR, int>::iterator edgeIDIt = activeAnimEdgeTimes.begin();
	for (; edgeIDIt != activeAnimEdgeTimes.end(); ++edgeIDIt)
	{
		int brightTime = edgeIDIt->second;
		if (brightTime == KEEP_BRIGHT) 
		{ 
			edge_data *pulsingEdge;
			assert(edge_exists(edgeIDIt->first, &pulsingEdge));

			unsigned int colArrIndex = pulsingEdge->arraypos + AOFF;
			int numEdgeVerts = pulsingEdge->vertSize;
			GLfloat *ecol = &animlinedata->acquire_col_write()->at(0);
			for (int i = 1; i < numEdgeVerts; ++i)
			{
				const unsigned int colArrIndex = pulsingEdge->arraypos + i*COLELEMS + AOFF;
				if (colArrIndex >= animlinedata->col_buf_capacity_floats())
				{
					animlinedata->release_col_write();
					break;
				}
				ecol[colArrIndex] = getPulseAlpha();
			}
			animlinedata->release_col_write();
			continue;
		}

		if (--edgeIDIt->second <= 0)
		{
			fadingAnimEdges.insert(edgeIDIt->first);
			edgeIDIt = activeAnimEdgeTimes.erase(edgeIDIt);
			if (edgeIDIt == activeAnimEdgeTimes.end()) break;
		}
	}
}

void thread_graph_data::redraw_anim_edges()
{
	map<NODEPAIR, int>::iterator edgeIDIt = activeAnimEdgeTimes.begin();
	for (; edgeIDIt != activeAnimEdgeTimes.end(); ++edgeIDIt)
	{
		NODEPAIR nodePair = edgeIDIt->first;

		GLfloat *ecol = &animlinedata->acquire_col_write()->at(0);

		edge_data *linkingEdge;
		assert(edge_exists(nodePair, &linkingEdge));

		int numEdgeVerts = linkingEdge->vertSize;
		unsigned int colArrIndex = linkingEdge->arraypos + AOFF;
		for (int i = 0; i < numEdgeVerts; ++i)
			ecol[colArrIndex] = 1;

		animlinedata->release_col_write();
	}
}

//reduce alpha of fading verts and edges
//remove from darkening list if it hits minimum alpha limit
void thread_graph_data::darken_fading(float fadeRate)
{
	//darken fading verts
	//more nodes makes it take longer, possibly increase faderate on large fadinganimnodes.size()'s
	set<unsigned int>::iterator alphaPosIt = fadingAnimNodes.begin();
	for (; alphaPosIt != fadingAnimNodes.end(); ++alphaPosIt)
	{
		unsigned int nodeAlphaIndex = *alphaPosIt;

		GLfloat *ncol = &animnodesdata->acquire_col_write()->at(0);
		//set alpha value to 1 in animation colour data
		float currentAlpha = ncol[nodeAlphaIndex];
		currentAlpha = fmax(ANIM_INACTIVE_NODE_ALPHA, currentAlpha - fadeRate);
		ncol[nodeAlphaIndex] = currentAlpha;
		animnodesdata->release_col_write();

		if (currentAlpha == ANIM_INACTIVE_NODE_ALPHA)
		{
			alphaPosIt = fadingAnimNodes.erase(alphaPosIt);
			if (alphaPosIt == fadingAnimNodes.end()) break;
		}
	}

	//darken fading edges
	set<NODEPAIR>::iterator edgeIDIt = fadingAnimEdges.begin();
	for (; edgeIDIt != fadingAnimEdges.end(); ++edgeIDIt)
	{
		NODEPAIR nodePair = *edgeIDIt;

		GLfloat *ecol = &animlinedata->acquire_col_write()->at(0);

		edge_data *linkingEdge;
		assert(edge_exists(nodePair, &linkingEdge));

		unsigned int colArrIndex = linkingEdge->arraypos + AOFF;
		float currentAlpha = ecol[colArrIndex];
		currentAlpha = fmax(ANIM_INACTIVE_EDGE_ALPHA, currentAlpha - fadeRate);
		ecol[colArrIndex] = currentAlpha;

		int numEdgeVerts = linkingEdge->vertSize;
		//set alpha value to 1 in animation colour data
		for (int i = 1; i < numEdgeVerts; ++i)
		{
			const unsigned int colArrIndex = linkingEdge->arraypos + i*COLELEMS + AOFF;
			if (colArrIndex >= animlinedata->col_buf_capacity_floats())
			{
				animlinedata->release_col_write();
				break;
			}
			ecol[colArrIndex] = currentAlpha;
		}
		animlinedata->release_col_write();

		if (currentAlpha == ANIM_INACTIVE_EDGE_ALPHA)
		{
			edgeIDIt = fadingAnimEdges.erase(edgeIDIt);
			if (edgeIDIt == fadingAnimEdges.end()) break;
		}
	}
}

void thread_graph_data::brighten_new_active()
{
	//brighten any new verts
	map<NODEINDEX, int>::iterator vertIDIt = newAnimNodeTimes.begin();
	for (; vertIDIt != newAnimNodeTimes.end(); ++vertIDIt)
	{
		NODEINDEX nodeIdx = vertIDIt->first;
		int animTime = vertIDIt->second;

		GLfloat *ncol = &animnodesdata->acquire_col_write()->at(0);
		

		const unsigned int arrIndexNodeAlpha = (nodeIdx * COLELEMS) + AOFF;
		if (arrIndexNodeAlpha >= animnodesdata->col_buf_capacity_floats())
		{
			//trying to brighten nodes we havent rendered yet
			animnodesdata->release_col_write();
			break;
		}

		//set alpha value to 1 in animation colour data
		ncol[arrIndexNodeAlpha] = 1;
		animnodesdata->release_col_write();

		//want to delay fading if in loop/unchained area, 
		if (animTime)
		{
			activeAnimNodeTimes[arrIndexNodeAlpha] = animTime;
			set <unsigned int>::iterator fadeIt = fadingAnimNodes.find(arrIndexNodeAlpha);
			if (fadeIt != fadingAnimNodes.end()) 
				fadingAnimNodes.erase(fadeIt);
		}
		else
			fadingAnimNodes.insert(arrIndexNodeAlpha);

		vertIDIt = newAnimNodeTimes.erase(vertIDIt);
		if (vertIDIt == newAnimNodeTimes.end()) break;
	}
	
	map <NODEINDEX, int>::iterator externTimeIt = newExternTimes.begin();
	for (; externTimeIt != newExternTimes.end(); ++externTimeIt)
	{
		NODEINDEX externNodeIdx = externTimeIt->first;

		unsigned int callsSoFar = callCounter[externNodeIdx]++;
		
		getNodeReadLock();
		node_data *externNode = unsafe_get_node(externNodeIdx);
		ARGLIST *args;
		obtainMutex(funcQueueMutex, 2121);
		if (externNode->funcargs.size() > callsSoFar)
			args = &externNode->funcargs.at(callsSoFar);
		else
			args = 0;

		MEM_ADDRESS insaddr = externNode->address;
		int nodeModule = externNode->nodeMod;
		dropNodeReadLock();
		
		string externString = generate_funcArg_string(get_node_sym(externNodeIdx, piddata), args);
		dropMutex(funcQueueMutex);

		string modPath;
		piddata->get_modpath(nodeModule, &modPath);

		stringstream callLogEntry;
		callLogEntry << "0x" << std::hex << insaddr << ": ";
		callLogEntry << modPath << " -> ";
		callLogEntry << externString << "\n";
		loggedCalls.push_back(callLogEntry.str());

		EXTTEXT extEntry;
		extEntry.framesRemaining = externTimeIt->second;
		extEntry.displayString = externString;
		extEntry.yOffset = 10;

		activeExternTimes[externNodeIdx] = extEntry;

		externTimeIt = newExternTimes.erase(externTimeIt);
		if (externTimeIt == newExternTimes.end()) break;
	}
	
	//brighten any new edges
	map<NODEPAIR, int>::iterator edgeIDIt = newAnimEdgeTimes.begin();
	for (; edgeIDIt != newAnimEdgeTimes.end(); ++edgeIDIt)
	{
		NODEPAIR nodePair = edgeIDIt->first;
		unsigned int animTime = edgeIDIt->second;

		GLfloat *ecol = &animlinedata->acquire_col_write()->at(0);

		edge_data *linkingEdge;
		if (!edge_exists(nodePair, &linkingEdge)) {
			cerr << "[rgat]WARNING: brightening new edges non-existant edge "<< nodePair.first << "," << nodePair.second<<endl;
			animlinedata->release_col_write();
			break;
		}

		//set alpha value to 1 in animation colour data
		int numEdgeVerts = linkingEdge->vertSize;
		for (int i = 0; i < numEdgeVerts; ++i)
		{
			const unsigned int colArrIndex = linkingEdge->arraypos + i*COLELEMS + AOFF;
			if (colArrIndex >= animlinedata->col_buf_capacity_floats())
			{
				animlinedata->release_col_write();
				break;
			}
			ecol[colArrIndex] = (float)1.0;
		}
		animlinedata->release_col_write();

		//want to delay fading if in loop/unchained area, 
		if (animTime)
		{
			activeAnimEdgeTimes[nodePair] = animTime;
			set <NODEPAIR>::iterator fadeIt = fadingAnimEdges.find(nodePair);

			if (fadeIt != fadingAnimEdges.end())
				fadingAnimEdges.erase(fadeIt);
		}
		else
			fadingAnimEdges.insert(nodePair);

		edgeIDIt = newAnimEdgeTimes.erase(edgeIDIt);
		if (edgeIDIt == newAnimEdgeTimes.end()) break;
	}
}


void thread_graph_data::render_animation(float fadeRate)
{
	brighten_new_active();
	maintain_active();
	darken_fading(fadeRate);

	//set_node_alpha(get_node(latest_active_node_idx)->index, animnodesdata, getPulseAlpha());

	//live process always at least has pulsing active node
	needVBOReload_active = true;
}


void thread_graph_data::render_live_animation(float fadeRate)
{
	process_live_animation_updates();
	render_animation(fadeRate);
}


int thread_graph_data::render_replay_animation(int stepSize, float fadeRate)
{
	int result = process_replay_animation_updates(stepSize);
	render_animation(fadeRate);
	return result;
}
/*
//find the node corresponding to the lateset instruction in the animation sequence
unsigned int thread_graph_data::derive_anim_node()
{

	obtainMutex(animationListsMutex, 1022);
	pair<MEM_ADDRESS, int> addr_size = bbsequence.at(sequenceIndex);
	BLOCK_IDENTIFIER blockID = mutationSequence.at(sequenceIndex);
	dropMutex(animationListsMutex);

	MEM_ADDRESS blockAddr = addr_size.first;
	int remainingInstructions = blockInstruction;
	
	INS_DATA *target_ins = getDisassemblyBlock(blockAddr, blockID, piddata, &terminationFlag)->at(blockInstruction);

	//this check is needed on early termination
	unordered_map<PID_TID, NODEINDEX>::iterator foundIdx = target_ins->threadvertIdx.find(tid);
	if (foundIdx != target_ins->threadvertIdx.end())
		return foundIdx->second;
	else
		return 0;
}
*/
void thread_graph_data::reset_mainlines() 
{
	mainlinedata->reset();
	animlinedata->reset();
}

//find the edge represented by pair of nodes 'edge'
//false if not found
//true if found + edge data placed in edged
bool thread_graph_data::edge_exists(NODEPAIR edge, edge_data **edged)
{

	getEdgeReadLock();
	EDGEMAP::iterator edgeit = edgeDict.find(edge);
	dropEdgeReadLock();

	if (edgeit == edgeDict.end()) return false;

	if (edged)
		*edged = &edgeit->second;
	return true;
}

edge_data *thread_graph_data::get_edge_create(node_data *source, node_data *target)
{
	NODEPAIR edge;
	edge.first = source->index;
	edge.second = target->index;

	getEdgeReadLock();
	EDGEMAP::iterator edgeDIt = edgeDict.find(edge);
	dropEdgeReadLock();

	if (edgeDIt != edgeDict.end())
		return &edgeDIt->second;

	edge_data edgeData;
	edgeData.edgeClass = INEW; //TODO!
	edgeData.chainedWeight = 0;
	add_edge(edgeData, source, target);

	return &edgeDict.at(edge);
}

inline edge_data *thread_graph_data::get_edge(NODEPAIR edgePair)
{
	getEdgeReadLock();
	EDGEMAP::iterator edgeIt = edgeDict.find(edgePair);
	dropEdgeReadLock();
	
	if (edgeIt != edgeDict.end())
		return &edgeIt->second;
	else
		return 0;
}


inline void thread_graph_data::getEdgeReadLock()
{
#ifdef XP_COMPATIBLE 
	obtainMutex(edMutex, 10001);
#else
	AcquireSRWLockShared(&edgeLock);
#endif
}

inline void thread_graph_data::getEdgeWriteLock()
{
#ifdef XP_COMPATIBLE 
	obtainMutex(edMutex, 10002);
#else
	AcquireSRWLockExclusive(&edgeLock);
#endif
}

inline void thread_graph_data::dropEdgeReadLock()
{
#ifdef XP_COMPATIBLE 
	dropMutex(edMutex);
#else
	ReleaseSRWLockShared(&edgeLock);
#endif
}

inline void thread_graph_data::dropEdgeWriteLock()
{
#ifdef XP_COMPATIBLE 
	dropMutex(edMutex);
#else
	ReleaseSRWLockExclusive(&edgeLock);
#endif
	
}

inline void thread_graph_data::getNodeReadLock()
{
#ifdef XP_COMPATIBLE 
	obtainMutex(nodeLMutex, 10005);
#else
	AcquireSRWLockShared(&nodeLock);
#endif
}

inline void thread_graph_data::dropNodeReadLock()
{
#ifdef XP_COMPATIBLE 
	dropMutex(nodeLMutex);
#else
	ReleaseSRWLockShared(&nodeLock);
#endif
}

inline void thread_graph_data::getNodeWriteLock()
{
#ifdef XP_COMPATIBLE 
	obtainMutex(nodeLMutex, 10006);
#else
	AcquireSRWLockExclusive(&nodeLock);
#endif
}

inline void thread_graph_data::dropNodeWriteLock()
{
#ifdef XP_COMPATIBLE 
	dropMutex(nodeLMutex);
#else
	ReleaseSRWLockExclusive(&nodeLock);
#endif
}

//linker error if we make this inline too
edge_data * thread_graph_data::get_edge(unsigned int edgeindex)
{
	if (edgeindex >= edgeList.size()) return 0;

	getEdgeReadLock();
	EDGEMAP::iterator edgeIt = edgeDict.find(edgeList.at(edgeindex));
	dropEdgeReadLock();

	if (edgeIt != edgeDict.end())
		return &edgeIt->second;
	else
		return 0;

}

inline node_data *thread_graph_data::safe_get_node(unsigned int index)
{
	getNodeReadLock();
	node_data *n = &nodeList.at(index);
	dropNodeReadLock();
	return n;
}

//for when caller already has read/write lock
node_data *thread_graph_data::unsafe_get_node(unsigned int index)
{
	return &nodeList.at(index);
}

void thread_graph_data::set_active_node(unsigned int idx)
{
	if (nodeList.size() <= idx) return;
	getNodeWriteLock();
	latest_active_node_idx = idx;
	latest_active_node_coord = unsafe_get_node(idx)->vcoord;
	dropNodeWriteLock();
}

//IMPORTANT: Must have edge reader lock to call this
int thread_graph_data::render_edge(NODEPAIR ePair, GRAPH_DISPLAY_DATA *edgedata, map<int, ALLEGRO_COLOR> *lineColours,
	ALLEGRO_COLOR *forceColour, bool preview)
{

	edge_data *e = &edgeDict.at(ePair);

	if (!e) return 0;

	MULTIPLIERS *scaling;
	if (preview)
		scaling = p_scalefactors;
	else
		scaling = m_scalefactors;

	FCOORD srcc = safe_get_node(ePair.first)->sphereCoordB(scaling, 0);
	FCOORD targc = safe_get_node(ePair.second)->sphereCoordB(scaling, 0);

	int arraypos = 0;
	ALLEGRO_COLOR *edgeColour;
	if (forceColour) edgeColour = forceColour;
	else
	{
		assert((size_t)e->edgeClass < lineColours->size());
		edgeColour = &lineColours->at(e->edgeClass);
	}

	int vertsDrawn = drawCurve(edgedata, &srcc, &targc,
		edgeColour, e->edgeClass, scaling, &arraypos);

	if (!preview)
	{
		e->vertSize = vertsDrawn;
		e->arraypos = arraypos;
	}

	return 1;

}

VCOORD *thread_graph_data::get_active_node_coord()
{
	if (nodeList.empty()) return NULL;

	getNodeReadLock();
	VCOORD *result = &latest_active_node_coord;
	dropNodeReadLock();

	return result;
}

thread_graph_data::thread_graph_data(PROCESS_DATA *processdata, unsigned int threadID)
{
	piddata = processdata;
	pid = piddata->PID;
	tid = threadID;

	mainnodesdata = new GRAPH_DISPLAY_DATA();
	mainlinedata = new GRAPH_DISPLAY_DATA();

	animlinedata = new GRAPH_DISPLAY_DATA();
	animnodesdata = new GRAPH_DISPLAY_DATA();

	previewlines = new GRAPH_DISPLAY_DATA(true);
	previewnodes = new GRAPH_DISPLAY_DATA(true);

	conditionallines = new GRAPH_DISPLAY_DATA();
	conditionalnodes = new GRAPH_DISPLAY_DATA();
	heatmaplines = new GRAPH_DISPLAY_DATA();
	needVBOReload_conditional = true;
	needVBOReload_heatmap = true;
	needVBOReload_main = true;
	needVBOReload_preview = true;
	m_scalefactors = new MULTIPLIERS;
	p_scalefactors = new MULTIPLIERS;
	p_scalefactors->HEDGESEP = 0.15;
	p_scalefactors->VEDGESEP = 0.11;
	p_scalefactors->radius = 200;
	p_scalefactors->baseRadius = 200;
}


void thread_graph_data::start_edgeL_iteration(EDGELIST::iterator *edgeIt, EDGELIST::iterator *edgeEnd)
{
	getEdgeReadLock();
	*edgeIt = edgeList.begin();
	*edgeEnd = edgeList.end();
}

void thread_graph_data::stop_edgeL_iteration()
{
	dropEdgeReadLock();
}

void thread_graph_data::start_edgeD_iteration(EDGEMAP::iterator *edgeIt,
	EDGEMAP::iterator *edgeEnd)
{
	getEdgeReadLock();
	*edgeIt = edgeDict.begin();
	*edgeEnd = edgeDict.end();
}

void thread_graph_data::stop_edgeD_iteration()
{
	dropEdgeReadLock();
}

void thread_graph_data::display_highlight_lines(vector<node_data *> *nodePtrList, ALLEGRO_COLOR *colour, int lengthModifier)
{
	int nodeListSize = nodePtrList->size();
	for (int nodeIdx = 0; nodeIdx != nodeListSize; ++nodeIdx)
		drawHighlight(&nodePtrList->at(nodeIdx)->vcoord, m_scalefactors, colour, lengthModifier);
}

void thread_graph_data::insert_node(NODEINDEX targVertID, node_data node)
{
	if (!nodeList.empty()) assert(targVertID == nodeList.back().index + 1);

	getNodeWriteLock();
	nodeList.push_back(node);
	dropNodeWriteLock();
}


void thread_graph_data::add_edge(edge_data e, node_data *source, node_data *target)
{
	NODEPAIR edgePair;
	edgePair.first = source->index;
	edgePair.second = target->index;

	getNodeWriteLock();

	source->outgoingNeighbours.insert(edgePair.second);
	if (source->conditional && (source->conditional != CONDCOMPLETE))
	{
		if (source->ins->condDropAddress == target->address)
			source->conditional |= CONDFELLTHROUGH;
		else if (source->ins->condTakenAddress == target->address)
			source->conditional |= CONDTAKEN;
	}

	target->incomingNeighbours.insert(edgePair.first);
	dropNodeWriteLock();

	getEdgeWriteLock();
	edgeDict.insert(make_pair(edgePair, e));
	edgeList.push_back(edgePair);
	dropEdgeWriteLock();
}

thread_graph_data::~thread_graph_data()
{
	delete animlinedata;
	delete animnodesdata;
}

//unused, should it be?
void thread_graph_data::set_edge_alpha(NODEPAIR eIdx, GRAPH_DISPLAY_DATA *edgesdata, float alpha)
{
	if (!edgesdata->get_numVerts()) return;
	edge_data *e = get_edge(eIdx);
	if (!e) return; 
	const unsigned int bufsize = edgesdata->col_buf_capacity_floats();
	GLfloat *colarray = &edgesdata->acquire_col_write()->at(0);
	for (unsigned int i = 0; i < e->vertSize; ++i)
	{
		unsigned int bufIndex = e->arraypos + i*COLELEMS + AOFF;
		if (bufIndex > bufsize) break;
		colarray[bufIndex] = alpha;
	}
	edgesdata->release_col_write();
}

//unused, should it be?
void thread_graph_data::set_node_alpha(unsigned int nIdx, GRAPH_DISPLAY_DATA *nodesdata, float alpha)
{
	unsigned int bufIndex = nIdx*COLELEMS + AOFF;
	if (bufIndex >= nodesdata->col_buf_capacity_floats()) return;

	GLfloat *colarray = &nodesdata->acquire_col_write()->at(0);
	colarray[bufIndex] = alpha;
	nodesdata->release_col_write();
}

void thread_graph_data::assign_modpath(PROCESS_DATA *pidinfo) 
{
	baseMod = safe_get_node(0)->nodeMod;
	if (baseMod >= (int)pidinfo->modpaths.size()) return;
	string longmodPath;
	pidinfo->get_modpath(baseMod, &longmodPath);

	if (longmodPath.size() > MAX_DIFF_PATH_LENGTH)
		modPath = ".."+longmodPath.substr(longmodPath.size() - MAX_DIFF_PATH_LENGTH, longmodPath.size());
	else
		modPath = longmodPath;
}

bool thread_graph_data::serialise(ofstream *file)
{
	*file << "TID" << tid << "{";

	*file << "N{";
	vector<node_data>::iterator vertit = nodeList.begin();
	for (; vertit != nodeList.end(); ++vertit)
		vertit->serialise(file);
	*file << "}N,";

	*file << "D{";
	EDGELIST::iterator edgeLIt = edgeList.begin();
	for (; edgeLIt != edgeList.end(); ++edgeLIt)
	{
		edge_data *e = get_edge(*edgeLIt);
		assert(e);
		e->serialise(file, edgeLIt->first, edgeLIt->second);
	}
	*file << "}D,";

	*file << "E{";
	vector<unsigned int>::iterator externit = externList.begin();
	for (; externit != externList.end(); ++externit)
		*file << *externit << ",";
	*file << "}E,";

	*file << "X{";
	set<unsigned int>::iterator exceptit = exceptionSet.begin();
	for (; exceptit != exceptionSet.end(); ++exceptit)
		*file << *exceptit << ",";
	*file << "}X,";

	//S for stats
	*file << "S{" 
		<< maxA << ","
		<< maxB << ","
		//<< loopCounter << ","
		<< baseMod << ","
		<< totalInstructions
		<< "}S,";

	/*
	*file << "A{";
	obtainMutex(animationListsMutex, 1030);
	for (unsigned long i = 0; i < bbsequence.size(); ++i)
	{
		pair<MEM_ADDRESS, int> seq_size = bbsequence.at(i);
		int mutation = mutationSequence.at(i);

		*file << seq_size.first << "," << seq_size.second << ","
			<< mutationSequence[i] << ","
			<< loopStateList[i].first << ",";
		if (loopStateList[i].first )
			*file << loopStateList[i].second << ",";
	}
	dropMutex(animationListsMutex);
	*file << "}A,";
	*/
	*file << "}";
	return true;
}

void thread_graph_data::push_anim_update(ANIMATIONENTRY entry)
{
	obtainMutex(animationListsMutex, 2412);
	animUpdates.push(entry);
	dropMutex(animationListsMutex);
}

bool thread_graph_data::loadEdgeDict(ifstream *file)
{
	string index_s, source_s, target_s, edgeclass_s;
	int source, target;
	while (true)
	{
		edge_data *edge = new edge_data;

		getline(*file, source_s, ',');
		if (!caught_stoi(source_s, (int *)&source, 10))
		{
			if (source_s == string("}D"))
				return true;
			else
				return false;
		}
		getline(*file, target_s, ',');
		if (!caught_stoi(target_s, (int *)&target, 10)) return false;
		getline(*file, edgeclass_s, '@');
		edge->edgeClass = edgeclass_s.c_str()[0];
		NODEPAIR stpair = make_pair(source, target);
		add_edge(*edge, safe_get_node(source), safe_get_node(target));
	}
	return false;
}

bool thread_graph_data::loadExterns(ifstream *file)
{
	string endtag;
	getline(*file, endtag, '{');
	if (endtag.c_str()[0] != 'E') return false;

	int index;
	string index_s;

	while (true) {
		getline(*file, index_s, ',');
		if (!caught_stoi(index_s, (int *)&index, 10))
		{
			if (index_s == string("}E")) return true;
			return false;
		}
		externList.push_back(index);
	}
}

bool thread_graph_data::loadExceptions(ifstream *file)
{
	string endtag;
	getline(*file, endtag, '{');
	if (endtag.c_str()[0] != 'X') return false;

	unsigned int index;
	string index_s;

	while (true) {
		getline(*file, index_s, ',');
		if (!caught_stoi(index_s, (int *)&index, 10))
		{
			if (index_s == string("}X")) return true;
			return false;
		}
		exceptionSet.insert(exceptionSet.end(),index);
	}
}


bool thread_graph_data::unserialise(ifstream *file, map <MEM_ADDRESS, INSLIST> *disassembly)
{
	if (!loadNodes(file, disassembly)) { cerr << "[rgat]ERROR:Node load failed"<<endl;  return false; }
	if (!loadEdgeDict(file)) { cerr << "[rgat]ERROR:EdgeD load failed" << endl; return false; }
	if (!loadExterns(file)) { cerr << "[rgat]ERROR:Externs load failed" << endl;  return false; }
	if (!loadExceptions(file)) { cerr << "[rgat]ERROR:Exceptions load failed" << endl;  return false; }
	if (!loadStats(file)) { cerr << "[rgat]ERROR:Stats load failed" << endl;  return false; }
	//if (!loadAnimationData(file)) { cerr << "[rgat]ERROR:Animation load failed" << endl;  return false; }
	return true;
}

bool thread_graph_data::loadNodes(ifstream *file, map <MEM_ADDRESS, INSLIST> *disassembly)
{

	if (!verifyTag(file, tag_START, 'N')) {
		cerr << "[rgat]Bad node data" << endl;
		return false;
	}
	string value_s;
	while (true)
	{
		node_data *n = new node_data;
		int result = n->unserialise(file, disassembly);
		if (result > 0)
		{
			insert_node(n->index, *n);
			continue;
		}

		delete n;

		if (!result) return true;
		else return false;
		
	}
}

//todo: move this and the other graph loads to graph class!
bool thread_graph_data::loadStats(ifstream *file)
{
	string endtag;
	getline(*file, endtag, '{');
	if (endtag.c_str()[0] != 'S') return false;

	string value_s;
	getline(*file, value_s, ',');
	if (!caught_stoi(value_s, &maxA, 10)) return false;
	getline(*file, value_s, ',');
	if (!caught_stoi(value_s, &maxB, 10)) return false;
	//getline(*file, value_s, ',');
	//if (!caught_stoi(value_s, (int *)&loopCounter, 10)) return false;
	getline(*file, value_s, ',');
	if (!caught_stoi(value_s, (int *)&baseMod, 10)) return false;
	getline(*file, value_s, '}');
	if (!caught_stoul(value_s, (unsigned long*)&totalInstructions, 10)) return false;

	getline(*file, endtag, ',');
	if (endtag.c_str()[0] != 'S') return false;
	return true;
}

/*
bool thread_graph_data::loadAnimationData(ifstream *file)
{
	string endtag;
	getline(*file, endtag, '{');
	if (endtag.c_str()[0] != 'A') return false;

	string sequence_s, size_s, mutation_s, loopstateIdx_s, loopstateIts_s;
	pair<MEM_ADDRESS, int> seq_size;
	pair<unsigned int, unsigned long> loopstateIdx_Its;
	BLOCK_IDENTIFIER blockID;

	while (true)
	{
		getline(*file, sequence_s, ',');
		if (sequence_s == "}A") 
		{ 
			//no trace data, assume graph was created in basic mode
			if (bbsequence.empty())
				basic = true;
			return true; 
		}
		if (!caught_stoul(sequence_s, &seq_size.first, 10)) break;
		getline(*file, size_s, ',');
		if (!caught_stoi(size_s, &seq_size.second, 10)) break;
		bbsequence.push_back(seq_size);

		getline(*file, mutation_s, ',');
		if (!caught_stoul(mutation_s, &blockID, 10)) break;

		mutationSequence.push_back(blockID);

		getline(*file, loopstateIdx_s, ',');
		if (!caught_stoi(loopstateIdx_s, (int *)&loopstateIdx_Its.first, 10)) break;
		if (loopstateIdx_Its.first)
		{
			getline(*file, loopstateIts_s, ',');
			if (!caught_stoul(loopstateIts_s, &loopstateIdx_Its.second, 10)) break;
		}
		else
			loopstateIdx_Its.second = 0xbad;

		loopStateList.push_back(loopstateIdx_Its);
	}
	return false;
}*/