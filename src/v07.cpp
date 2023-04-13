#include <chrono>
#include <deque>
#include <ios>
#include <iostream>
#include <numeric>
#include <ostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef LOCAL
#define log if (true) std::cerr
#else
#define log if (false) std::cerr
#endif

struct PM;
struct Rack;
struct Domain;
struct VM;

template<typename T>
std::ostream &operator<<(std::ostream &os, const std::vector<T> &vec) {
    os << '[';

    for (int i = 0; i < vec.size(); i++) {
        os << vec[i];

        if (i != vec.size() - 1) {
            os << ", ";
        }
    }

    os << ']';
    return os;
}

enum class Affinity {
    NONE = 0,
    SOFT = 1,
    HARD = 2
};

struct Type {
    int nodes;
    int cpu;
    int memory;

    Type(int nodes, int cpu, int memory) : nodes(nodes), cpu(cpu), memory(memory) {}
};

struct WithResources {
    int totalCPU;
    int totalMemory;

    int availableCPU;
    int availableMemory;

    WithResources(int cpu, int memory)
            : totalCPU(cpu), totalMemory(memory), availableCPU(cpu), availableMemory(memory) {}

    void claimResources(Type *type) {
        availableCPU -= type->cpu;
        availableMemory -= type->memory;
    }

    void releaseResources(Type *type) {
        availableCPU += type->cpu;
        availableMemory += type->memory;
    }

    [[nodiscard]] bool hasResources(int cpu, int memory) const {
        return availableCPU >= cpu && availableMemory >= memory;
    }

    [[nodiscard]] bool hasResources(const Type &type) const {
        return hasResources(type.nodes * type.cpu, type.nodes * type.memory);
    }

    [[nodiscard]] double getLoad() const {
        double loadCPU = (double) (totalCPU - availableCPU) / totalCPU;
        double loadMemory = (double) (totalMemory - availableMemory) / totalMemory;

        return std::max(loadCPU, loadMemory);
    }
};

struct Node : WithResources {
    int index;
    PM *pm;

    Node(int index, PM *pm, int cpu, int memory) : WithResources(cpu, memory), index(index), pm(pm) {}

    [[nodiscard]] int supportsOfType(const Type &type) const {
        return std::min(availableCPU / type.cpu, availableMemory / type.memory);
    }
};

struct PM : WithResources {
    int index;
    Rack *rack;

    std::vector<Node> nodes;
    std::unordered_map<int, int> vmsByPG;

    PM(int index, Rack *rack, const std::vector<int> &nodeCPU, const std::vector<int> &nodeMemory)
            : WithResources(std::accumulate(nodeCPU.begin(), nodeCPU.end(), 0),
                            std::accumulate(nodeMemory.begin(), nodeMemory.end(), 0)),
              index(index),
              rack(rack) {
        nodes.reserve(nodeCPU.size());
        for (int i = 0; i < nodeCPU.size(); i++) {
            nodes.emplace_back(i + 1, this, nodeCPU[i], nodeMemory[i]);
        }
    }

    [[nodiscard]] int supportsOfType(const Type &type) const {
        std::vector<int> byNode(nodes.size());
        for (int i = 0; i < nodes.size(); i++) {
            byNode[i] = nodes[i].supportsOfType(type);
        }

        std::sort(byNode.begin(), byNode.end());

        int count = 0;
        for (int i = 0; i < byNode.size(); i += type.nodes) {
            count += byNode[i];
        }

        return count;
    }
};

struct Rack : WithResources {
    int index;
    Domain *domain;

    std::vector<PM> pms;

    Rack(int index, Domain *domain, int noPMs, const std::vector<int> &nodeCPU, const std::vector<int> &nodeMemory)
            : WithResources(noPMs * std::accumulate(nodeCPU.begin(), nodeCPU.end(), 0),
                            noPMs * std::accumulate(nodeMemory.begin(), nodeMemory.end(), 0)),
              index(index),
              domain(domain) {
        pms.reserve(noPMs);
        for (int i = 0; i < noPMs; i++) {
            pms.emplace_back(i + 1, this, nodeCPU, nodeMemory);
        }
    }

    [[nodiscard]] int supportsOfType(const Type &type) const {
        int count = 0;

        for (const PM &pm : pms) {
            count += pm.supportsOfType(type);
        }

        return count;
    }
};

struct Domain : WithResources {
    int index;

    std::vector<Rack> racks;

    Domain(int index, int noRacks, int noPMs, const std::vector<int> &nodeCPU, const std::vector<int> &nodeMemory)
            : WithResources(noRacks * noPMs * std::accumulate(nodeCPU.begin(), nodeCPU.end(), 0),
                            noRacks * noPMs * std::accumulate(nodeMemory.begin(), nodeMemory.end(), 0)),
              index(index) {
        racks.reserve(noRacks);
        for (int i = 0; i < noRacks; i++) {
            racks.emplace_back(i + 1, this, noPMs, nodeCPU, nodeMemory);
        }
    }

    [[nodiscard]] int supportsOfType(const Type &type) const {
        int count = 0;

        for (const Rack &rack : racks) {
            count += rack.supportsOfType(type);
        }

        return count;
    }
};

struct PG {
    int index;
    int hardRackAntiAffinityPartitions;
    int softPMAntiAffinity;
    Affinity domainAffinity;
    Affinity rackAffinity;

    std::vector<VM *> vms;

    Domain *targetDomain = nullptr;
    bool domainAffinityPossible = true;

    Rack *targetRack = nullptr;
    bool rackAffinityPossible = true;

    bool softScorePossible = true;

    std::unordered_map<int, std::unordered_set<Rack *>> partitionRacks;

    PG(int index,
       int hardRackAntiAffinityPartitions,
       int softPMAntiAffinity,
       Affinity domainAffinity,
       Affinity rackAffinity)
            : index(index),
              hardRackAntiAffinityPartitions(hardRackAntiAffinityPartitions),
              softPMAntiAffinity(softPMAntiAffinity),
              domainAffinity(domainAffinity),
              rackAffinity(rackAffinity) {}

    void updateTargets();
};

struct VM {
    int index;
    Type *type;
    PG *pg;
    int partition;

    std::vector<Node *> nodes;

    VM(int index, Type *type, PG *pg, int partition) : index(index), type(type), pg(pg), partition(partition) {}

    void place(const std::vector<Node *> &placement) {
        nodes = placement;

        for (Node *node : nodes) {
            node->claimResources(type);
            node->pm->claimResources(type);
            node->pm->rack->claimResources(type);
            node->pm->rack->domain->claimResources(type);
        }

        nodes[0]->pm->vmsByPG[pg->index]++;
    }

    void unplace() {
        for (Node *node : nodes) {
            node->releaseResources(type);
            node->pm->releaseResources(type);
            node->pm->rack->releaseResources(type);
            node->pm->rack->domain->releaseResources(type);
        }

        nodes[0]->pm->vmsByPG[pg->index]--;

        nodes.clear();
    }

    [[nodiscard]] bool isPlaced() const {
        return !nodes.empty();
    }
};

void PG::updateTargets() {
    targetDomain = nullptr;
    domainAffinityPossible = true;

    targetRack = nullptr;
    rackAffinityPossible = true;

    partitionRacks.clear();

    for (VM *vm : vms) {
        if (!vm->isPlaced()) {
            continue;
        }

        Rack *rack = vm->nodes[0]->pm->rack;
        Domain *domain = rack->domain;

        if (domainAffinity != Affinity::NONE && domainAffinityPossible) {
            if (targetDomain == nullptr) {
                targetDomain = domain;
            } else if (targetDomain != domain) {
                domainAffinityPossible = false;
            }
        }

        if (rackAffinity != Affinity::NONE && rackAffinityPossible) {
            if (targetRack == nullptr) {
                targetRack = rack;
            } else if (targetRack != rack) {
                rackAffinityPossible = false;
            }
        }

        if (hardRackAntiAffinityPartitions > 0) {
            partitionRacks[vm->partition].insert(rack);
        }
    }

    softScorePossible = softPMAntiAffinity > 0
                        && ((domainAffinity == Affinity::NONE || domainAffinityPossible)
                            || (rackAffinity == Affinity::NONE || rackAffinityPossible));
}

struct Manager {
    int noDomains;
    int noRacks;
    int noPMs;
    int noNodes;
    int noTypes;

    std::vector<Type> types;

    std::vector<Domain> domains;

    std::unordered_map<int, PG> pgsByIndex;
    std::unordered_map<int, VM> vmsByIndex;

    std::chrono::high_resolution_clock::time_point startTime;

    Manager(int noDomains,
            int noRacks,
            int noPMs,
            const std::vector<int> &nodeCPU,
            const std::vector<int> &nodeMemory,
            const std::vector<Type> &types)
            : noDomains(noDomains),
              noRacks(noRacks),
              noPMs(noPMs),
              noNodes(nodeCPU.size()),
              noTypes(types.size()),
              types(types),
              startTime(std::chrono::high_resolution_clock::now()) {
        domains.reserve(noDomains);
        for (int i = 0; i < noDomains; i++) {
            domains.emplace_back(i + 1, noRacks, noPMs, nodeCPU, nodeMemory);
        }
    }

    void createPG(int index,
                  int hardRackAntiAffinityPartitions,
                  int softPMAntiAffinity,
                  Affinity domainAffinity,
                  Affinity rackAffinity) {
        if (hardRackAntiAffinityPartitions <= 1) {
            hardRackAntiAffinityPartitions = 0;
        }

        pgsByIndex.emplace(index,
                           PG(index, hardRackAntiAffinityPartitions, softPMAntiAffinity, domainAffinity, rackAffinity));
    }

    bool createVMs(const std::vector<int> &indices, int typeIndex, int pgIndex, int partition) {
        auto currentTime = std::chrono::high_resolution_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime).count() >= 14) {
            log << "Timed out, terminating" << std::endl;
            std::cout << -1 << std::endl;
            return false;
        }

        Type &type = types.at(typeIndex - 1);
        PG &pg = pgsByIndex.at(pgIndex);

        if (pg.hardRackAntiAffinityPartitions == 0) {
            partition = 0;
        }

        std::vector<VM *> vmsToPlace;

        for (int i = 0; i < indices.size(); i++) {
            vmsByIndex.emplace(indices[i], VM(indices[i], &type, &pg, partition >= 0 ? partition : i + 1));
            VM &vm = vmsByIndex.at(indices[i]);

            pg.vms.push_back(&vm);
            vmsToPlace.push_back(&vm);
        }

        int placed = 0;

        for (VM *vm : vmsToPlace) {
            std::vector<Node *> bestNodes;
            double bestPenalty;

            for (Rack *rack : getRacksToTry(vmsToPlace, *vm)) {
                if (!rack->hasResources(type)) {
                    continue;
                }

                for (PM &pm : rack->pms) {
                    if (!pm.hasResources(type)) {
                        continue;
                    }

                    std::vector<Node *> nodes;
                    for (Node &node : pm.nodes) {
                        if (!node.hasResources(type.cpu, type.memory)) {
                            continue;
                        }

                        nodes.push_back(&node);

                        if (nodes.size() == type.nodes) {
                            break;
                        }
                    }

                    if (nodes.size() != type.nodes) {
                        continue;
                    }

                    double penalty = 0;

                    penalty += 3 - (rack->domain->getLoad() + rack->getLoad() + pm.getLoad());

                    if (pg.domainAffinity == Affinity::SOFT
                        && pg.domainAffinityPossible
                        && pg.softScorePossible
                        && pg.targetDomain != nullptr
                        && rack->domain != pg.targetDomain) {
                        penalty += 1000;
                    }

                    if (pg.rackAffinity == Affinity::SOFT
                        && pg.rackAffinityPossible
                        && pg.softScorePossible
                        && pg.targetRack != nullptr
                        && rack != pg.targetRack) {
                        penalty += 1000;
                    }

                    if (pg.softPMAntiAffinity > 0
                        && pg.softScorePossible
                        && pm.vmsByPG[pg.index] == pg.softPMAntiAffinity) {
                        penalty++;

                        for (VM *otherVM : vmsToPlace) {
                            if (vm == otherVM || !otherVM->isPlaced()) {
                                continue;
                            }

                            if (otherVM->nodes[0]->pm == &pm) {
                                penalty++;
                            }
                        }
                    }

                    /*if (pg.hardRackAntiAffinityPartitions > 0 && !pg.partitionRacks[vm->partition].contains(rack)) {
                        penalty++;
                    }*/

                    if (bestNodes.empty() || penalty < bestPenalty) {
                        bestNodes = nodes;
                        bestPenalty = penalty;
                    }

                    if (!bestNodes.empty() && bestPenalty == 0) {
                        break;
                    }
                }

                if (!bestNodes.empty() && bestPenalty == 0) {
                    break;
                }
            }

            if (bestNodes.empty()) {
                break;
            }

            log << "VM " << vm->index << " best penalty: " << bestPenalty << std::endl;
            vm->place(bestNodes);
            placed++;
        }

        if (placed < vmsToPlace.size()) {
            log << "Cannot create " << (vmsToPlace.size() - placed) << " VM(s), terminating" << std::endl;
            std::cout << -1 << std::endl;
            return false;
        }

        for (VM *vm : vmsToPlace) {
            std::vector<Node *> &nodes = vm->nodes;
            PM *pm = nodes[0]->pm;

            std::cout << pm->rack->domain->index << ' ' << pm->rack->index << ' ' << pm->index;
            for (Node *node : nodes) {
                std::cout << ' ' << node->index;
            }
            std::cout << std::endl;
        }

        return true;
    }

    std::deque<Rack *> getRacksToTry(const std::vector<VM *> &vmsToPlace, const VM &vm) {
        PG *pg = vm.pg;
        pg->updateTargets();

        std::deque<Rack *> racks;

        if (pg->rackAffinity == Affinity::HARD) {
            if (pg->targetRack != nullptr) {
                racks.push_back(pg->targetRack);
            } else {
                for (Domain &domain : domains) {
                    for (Rack &rack : domain.racks) {
                        racks.push_back(&rack);
                    }
                }

                std::sort(racks.begin(), racks.end(), [](const Rack *a, const Rack *b) {
                    return a->getLoad() < b->getLoad();
                });
            }
        } else if (pg->domainAffinity == Affinity::HARD
                   && (pg->rackAffinity == Affinity::NONE || !pg->rackAffinityPossible)) {
            if (pg->targetDomain != nullptr) {
                for (Rack &rack : pg->targetDomain->racks) {
                    racks.push_back(&rack);
                }
            } else {
                for (Domain &domain : domains) {
                    for (Rack &rack : domain.racks) {
                        racks.push_back(&rack);
                    }
                }
            }

            std::sort(racks.begin(), racks.end(), [](const Rack *a, const Rack *b) {
                return a->getLoad() < b->getLoad();
            });
        } else if (pg->domainAffinity == Affinity::HARD
                   && pg->rackAffinity == Affinity::SOFT
                   && pg->rackAffinityPossible) {
            int sortStart = 0;

            if (pg->targetRack != nullptr) {
                racks.push_back(pg->targetRack);
                sortStart++;
            }

            if (pg->targetDomain != nullptr) {
                for (Rack &rack : pg->targetDomain->racks) {
                    if (&rack != pg->targetRack) {
                        racks.push_back(&rack);
                    }
                }
            } else {
                for (Domain &domain : domains) {
                    for (Rack &rack : domain.racks) {
                        if (&rack != pg->targetRack) {
                            racks.push_back(&rack);
                        }
                    }
                }
            }

            std::sort(racks.begin() + sortStart, racks.end(), [](const Rack *a, const Rack *b) {
                return a->getLoad() < b->getLoad();
            });
        } else if (pg->domainAffinity == Affinity::SOFT
                   && pg->domainAffinityPossible
                   && pg->rackAffinity == Affinity::SOFT
                   && pg->rackAffinityPossible) {
            int sortStart = 0;

            if (pg->targetRack != nullptr) {
                racks.push_back(pg->targetRack);
                sortStart++;
            }

            if (pg->targetDomain != nullptr) {
                for (Rack &rack : pg->targetDomain->racks) {
                    if (&rack != pg->targetRack) {
                        racks.push_back(&rack);
                    }
                }

                std::sort(racks.begin() + sortStart, racks.end(), [](const Rack *a, const Rack *b) {
                    return a->getLoad() < b->getLoad();
                });

                sortStart = racks.size();
            }

            for (Domain &domain : domains) {
                for (Rack &rack : domain.racks) {
                    if (std::find(racks.begin(), racks.end(), &rack) == racks.end()) {
                        racks.push_back(&rack);
                    }
                }
            }

            std::sort(racks.begin() + sortStart, racks.end(), [](const Rack *a, const Rack *b) {
                return a->getLoad() < b->getLoad();
            });
        } else if (pg->domainAffinity == Affinity::SOFT
                   && pg->domainAffinityPossible
                   && (pg->rackAffinity == Affinity::NONE || !pg->rackAffinityPossible)) {
            int sortStart = 0;

            if (pg->targetDomain != nullptr) {
                for (Rack &rack : pg->targetDomain->racks) {
                    racks.push_back(&rack);
                }

                std::sort(racks.begin(), racks.end(), [](const Rack *a, const Rack *b) {
                    return a->getLoad() < b->getLoad();
                });

                sortStart = racks.size();
            }

            for (Domain &domain : domains) {
                for (Rack &rack : domain.racks) {
                    if (std::find(racks.begin(), racks.end(), &rack) == racks.end()) {
                        racks.push_back(&rack);
                    }
                }
            }

            std::sort(racks.begin() + sortStart, racks.end(), [](const Rack *a, const Rack *b) {
                return a->getLoad() < b->getLoad();
            });
        } else if ((pg->domainAffinity == Affinity::NONE || !pg->domainAffinityPossible)
                   || (pg->rackAffinity == Affinity::NONE || !pg->rackAffinityPossible)) {
            for (Domain &domain : domains) {
                for (Rack &rack : domain.racks) {
                    racks.push_back(&rack);
                }
            }

            std::sort(racks.begin(), racks.end(), [](const Rack *a, const Rack *b) {
                return a->getLoad() < b->getLoad();
            });
        } else {
            log << "Invalid affinity combination" << std::endl;
        }

        if (pg->rackAffinity == Affinity::HARD && pg->targetRack == nullptr) {
            log << "Filter 1" << std::endl;
            racks.erase(std::remove_if(racks.begin(), racks.end(), [&](const Rack *rack) {
                return rack->supportsOfType(*vm.type) < vmsToPlace.size();
            }), racks.end());
        }

        if (pg->domainAffinity == Affinity::HARD && pg->targetDomain == nullptr) {
            log << "Filter 2" << std::endl;
            std::unordered_map<int, int> supportByDomain;

            for (Rack *rack : racks) {
                supportByDomain[rack->domain->index] += rack->supportsOfType(*vm.type);
            }

            racks.erase(std::remove_if(racks.begin(), racks.end(), [&](const Rack *rack) {
                return supportByDomain[rack->domain->index] < vmsToPlace.size();
            }), racks.end());
        }

        if (pg->hardRackAntiAffinityPartitions > 0) {
            for (auto &[partition, usedRacks] : pg->partitionRacks) {
                if (partition == vm.partition) {
                    std::vector<Rack *> usedRacksVector(usedRacks.begin(), usedRacks.end());
                    std::sort(usedRacksVector.begin(), usedRacksVector.end(), [](const Rack *a, const Rack *b) {
                        return a->getLoad() > b->getLoad();
                    });

                    for (Rack *rack : usedRacksVector) {
                        if (std::find(racks.begin(), racks.end(), rack) != racks.end()) {
                            racks.erase(std::remove(racks.begin(), racks.end(), rack), racks.end());
                            racks.push_front(rack);
                        }
                    }
                } else {
                    for (Rack *rack : usedRacks) {
                        racks.erase(std::remove(racks.begin(), racks.end(), rack), racks.end());
                    }
                }
            }
        }

        return racks;
    }

    void deleteVMs(const std::vector<int> &indices) {
        for (int index : indices) {
            VM &vm = vmsByIndex.at(index);
            vm.unplace();
            vm.pg->vms.erase(std::remove(vm.pg->vms.begin(), vm.pg->vms.end(), &vm), vm.pg->vms.end());
            vmsByIndex.erase(index);
        }
    }

    template<typename T>
    std::vector<T *> getVectorPointers(std::vector<T> &values) const {
        std::vector<T *> pointers(values.size());

        for (int i = 0; i < values.size(); i++) {
            pointers[i] = &values[i];
        }

        return pointers;
    }

    void unplaceVMs(const std::vector<VM *> &vms) const {
        for (VM *vm : vms) {
            if (vm->isPlaced()) {
                vm->unplace();
            }
        }
    }
};

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    int noDomains, noRacks, noPMs, noNodes;
    std::cin >> noDomains >> noRacks >> noPMs >> noNodes;

    log << "noDomains = " << noDomains
        << ", noRacks = " << noRacks
        << ", noPMs = " << noPMs
        << ", noNodes = " << noNodes
        << std::endl;

    std::vector<int> nodeCPU(noNodes);
    std::vector<int> nodeMemory(noNodes);
    for (int i = 0; i < noNodes; i++) {
        std::cin >> nodeCPU[i] >> nodeMemory[i];

        log << "Node " << (i + 1) << ": cpu = " << nodeCPU[i] << ", memory = " << nodeMemory[i] << std::endl;
    }

    int noTypes;
    std::cin >> noTypes;

    std::vector<Type> types;
    for (int i = 0; i < noTypes; i++) {
        int nodes, cpu, memory;
        std::cin >> nodes >> cpu >> memory;

        types.emplace_back(nodes, cpu, memory);

        log << "Type " << (i + 1) << ": nodes = " << nodes << ", cpu = " << cpu << ", memory = " << memory << std::endl;
    }

    Manager manager(noDomains, noRacks, noPMs, nodeCPU, nodeMemory, types);

    for (int requestId = 1;; requestId++) {
        log << "\nRequest " << requestId << ": ";

        int requestType;
        std::cin >> requestType;

        bool terminate = false;
        switch (requestType) {
            case 1: {
                int index, hardRackAntiAffinityPartitions, softPMAntiAffinity;
                std::cin >> index >> hardRackAntiAffinityPartitions >> softPMAntiAffinity;

                int domainAffinity, rackAffinity;
                std::cin >> domainAffinity >> rackAffinity;

                log << "Create PG\n"
                    << "index = " << index
                    << ", hardRackAntiAffinityPartitions = " << hardRackAntiAffinityPartitions
                    << ", softPMAntiAffinity = " << softPMAntiAffinity
                    << ", domainAffinity = " << domainAffinity
                    << ", rackAffinity = " << rackAffinity
                    << "\n----------"
                    << std::endl;

                manager.createPG(index,
                                 hardRackAntiAffinityPartitions,
                                 softPMAntiAffinity,
                                 static_cast<Affinity>(domainAffinity),
                                 static_cast<Affinity>(rackAffinity));
                break;
            }
            case 2: {
                int noVMs, typeIndex, pgIndex, partition;
                std::cin >> noVMs >> typeIndex >> pgIndex >> partition;

                std::vector<int> indices(noVMs);
                for (int i = 0; i < noVMs; i++) {
                    std::cin >> indices[i];
                }

                log << "Create VM(s)\n"
                    << "noVMs = " << noVMs
                    << ", typeIndex = " << typeIndex
                    << ", pgIndex = " << pgIndex
                    << ", partition = " << partition
                    << "\nindices = " << indices
                    << "\n----------"
                    << std::endl;

                terminate = !manager.createVMs(indices, typeIndex, pgIndex, partition);
                break;
            }
            case 3: {
                int noVMs;
                std::cin >> noVMs;

                std::vector<int> indices(noVMs);
                for (int i = 0; i < noVMs; i++) {
                    std::cin >> indices[i];
                }

                log << "Delete VM(s)\n"
                    << "noVMs = " << noVMs
                    << "\nindices = " << indices
                    << "\n----------"
                    << std::endl;

                manager.deleteVMs(indices);
                break;
            }
            case 4: {
                log << "Terminate\n----------" << std::endl;
                terminate = true;
                break;
            }
        }

        if (terminate) {
            break;
        }
    }

    return 0;
}
