#include <algorithm>
#include <ios>
#include <iostream>
#include <unordered_map>
#include <vector>

#ifdef LOCAL
#define log if (true) std::cerr
#else
#define log if (false) std::cerr
#endif

enum class Affinity {
    NONE,
    SOFT,
    HARD
};

struct Type {
    int nodes;
    int cpu;
    int memory;

    Type(int nodes, int cpu, int memory) : nodes(nodes), cpu(cpu), memory(memory) {}
};

struct PM;
struct Rack;
struct Domain;
struct VM;

struct Node {
    int index;
    PM *pm;

    int cpu;
    int memory;

    std::vector<VM *> vms;

    Node(int index, PM *pm, int cpu, int memory) : index(index), pm(pm), cpu(cpu), memory(memory) {}
};

struct PM {
    int index;
    Rack *rack;

    std::vector<Node> nodes;

    PM(int index, Rack *rack, const std::vector<int> &nodeCPU, const std::vector<int> &nodeMemory)
            : index(index), rack(rack) {
        int noNodes = nodeCPU.size();

        nodes.reserve(noNodes);
        for (int i = 0; i < noNodes; i++) {
            nodes.emplace_back(i + 1, this, nodeCPU[i], nodeMemory[i]);
        }
    }
};

struct Rack {
    int index;
    Domain *domain;

    std::vector<PM> pms;

    Rack(int index,
         Domain *domain,
         int noPMs,
         const std::vector<int> &nodeCPU,
         const std::vector<int> &nodeMemory)
            : index(index), domain(domain) {
        pms.reserve(noPMs);
        for (int i = 0; i < noPMs; i++) {
            pms.emplace_back(i + 1, this, nodeCPU, nodeMemory);
        }
    }
};

struct Domain {
    int index;

    std::vector<Rack> racks;

    Domain(int index,
           int noRacks,
           int noPMs,
           const std::vector<int> &nodeCPU,
           const std::vector<int> &nodeMemory)
            : index(index) {
        racks.reserve(noRacks);
        for (int i = 0; i < noRacks; i++) {
            racks.emplace_back(i + 1, this, noPMs, nodeCPU, nodeMemory);
        }
    }
};

struct PG {
    int index;
    int hardRackAntiAffinityPartitions;
    int softPMAntiAffinity;
    Affinity domainAffinity;
    Affinity rackAffinity;

    std::vector<VM *> vms;

    PG(int index,
       int hardRackAntiAffinityPartitions,
       int softMachineAntiAffinity,
       Affinity domainAffinity,
       Affinity rackAffinity)
            : index(index),
              hardRackAntiAffinityPartitions(hardRackAntiAffinityPartitions),
              softPMAntiAffinity(softMachineAntiAffinity),
              domainAffinity(domainAffinity),
              rackAffinity(rackAffinity) {}
};

struct VM {
    int index;
    Type *type;
    PG *pg;
    int partition;

    std::vector<Node *> nodes;

    VM(int index, Type *type, PG *pg, int partition) : index(index), type(type), pg(pg), partition(partition) {}
};

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
              types(types) {
        domains.reserve(noDomains);

        for (int i = 0; i < noDomains; i++) {
            domains.emplace_back(i + 1, noRacks, noPMs, nodeCPU, nodeMemory);
        }
    }

    void createPG(int index,
                  int hardRackAntiAffinityPartitions,
                  int softPMAntiAffinity,
                  Affinity networkAffinity,
                  Affinity rackAffinity) {
        pgsByIndex.emplace(index,
                           PG(index, hardRackAntiAffinityPartitions, softPMAntiAffinity, networkAffinity, rackAffinity));

    }

    bool createVMs(const std::vector<int> &indices, int typeIndex, int pgIndex, int partition) {
        auto &type = types[typeIndex - 1];
        auto &pg = pgsByIndex.at(pgIndex);

        std::vector<VM *> vmsToPlace;
        vmsToPlace.reserve(indices.size());

        for (int i = 0; i < indices.size(); i++) {
            int index = indices[i];
            vmsByIndex.emplace(index, VM(index, &type, &pg, partition >= 0 ? partition : i + 1));
            vmsToPlace.push_back(&vmsByIndex.at(index));
        }

        std::unordered_map<VM *, std::vector<Node *>> placement;

        for (auto *vm : vmsToPlace) {
            bool foundPlace = false;

            for (auto &domain : domains) {
                for (auto &rack : domain.racks) {
                    if (!rackSupportsVM(rack, vm, pg)) {
                        continue;
                    }

                    for (auto &pm : rack.pms) {
                        std::vector<Node *> nodes;

                        for (auto &node : pm.nodes) {
                            if (node.cpu >= type.cpu && node.memory >= type.memory) {
                                nodes.push_back(&node);

                                if (nodes.size() == type.nodes) {
                                    break;
                                }
                            }
                        }

                        if (nodes.size() != type.nodes) {
                            continue;
                        }

                        vm->nodes = nodes;
                        for (auto *node : nodes) {
                            node->cpu -= type.cpu;
                            node->memory -= type.memory;
                            node->vms.push_back(vm);
                        }

                        pg.vms.push_back(vm);

                        placement[vm] = nodes;
                        foundPlace = true;
                        break;
                    }

                    if (foundPlace) {
                        break;
                    }
                }

                if (foundPlace) {
                    break;
                }
            }

            if (!foundPlace) {
                std::cout << -1 << std::endl;
                return false;
            }
        }

        for (auto *vm : vmsToPlace) {
            const auto &nodes = placement.at(vm);
            const auto *firstNode = nodes.at(0);

            const auto *domain = firstNode->pm->rack->domain;
            const auto *rack = firstNode->pm->rack;
            const auto *pm = firstNode->pm;

            std::cout << domain->index << " " << rack->index << " " << pm->index;
            for (const auto *node : nodes) {
                std::cout << " " << node->index;
            }
            std::cout << std::endl;
        }

        return true;
    }

    bool rackSupportsVM(Rack &rack, VM *vm, PG &pg) {
        if (pg.vms.empty()) {
            return true;
        }

        if (pg.domainAffinity == Affinity::HARD) {
            if (pg.vms[0]->nodes[0]->pm->rack->domain != rack.domain) {
                return false;
            }
        }

        if (pg.rackAffinity == Affinity::HARD) {
            if (pg.vms[0]->nodes[0]->pm->rack != &rack) {
                return false;
            }
        }

        if (pg.hardRackAntiAffinityPartitions > 0) {
            for (auto *otherVM : pg.vms) {
                if (otherVM->nodes[0]->pm->rack == &rack && otherVM->partition != vm->partition) {
                    return false;
                }
            }
        }

        return true;
    }

    void deleteVMs(const std::vector<int> &indices) {
        for (int index : indices) {
            auto &vm = vmsByIndex.at(index);

            for (auto *node : vm.nodes) {
                node->cpu += vm.type->cpu;
                node->memory += vm.type->memory;
                removeFromVector(node->vms, &vm);
            }

            removeFromVector(vm.pg->vms, &vm);
            vmsByIndex.erase(index);
        }
    }

    template<typename T>
    void removeFromVector(std::vector<T *> &vector, T *value) {
        vector.erase(std::remove(vector.begin(), vector.end(), value), vector.end());
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

        log << "Type " << (i + 1) << ": "
            << "nodes = " << nodes
            << ", cpu = " << cpu
            << ", memory = " << memory
            << std::endl;

        types.emplace_back(nodes, cpu, memory);
    }

    Manager manager(noDomains, noRacks, noPMs, nodeCPU, nodeMemory, types);

    for (int i = 0;; i++) {
        log << "\nRequest " << (i + 1) << ": ";

        int requestType;
        std::cin >> requestType;

        bool terminate = false;
        switch (requestType) {
            case 1: {
                int index, hardRackAntiAffinityPartitions, softPMAntiAffinity;
                std::cin >> index >> hardRackAntiAffinityPartitions >> softPMAntiAffinity;

                int networkAffinity, rackAffinity;
                std::cin >> networkAffinity >> rackAffinity;

                log << "Create PG\n"
                    << "index = " << index
                    << ", hardRackAntiAffinityPartitions = " << hardRackAntiAffinityPartitions
                    << ", softPMAntiAffinity = " << softPMAntiAffinity
                    << ", networkAffinity = " << networkAffinity
                    << ", rackAffinity = " << rackAffinity
                    << "\n----------"
                    << std::endl;

                manager.createPG(index,
                                 hardRackAntiAffinityPartitions,
                                 softPMAntiAffinity,
                                 static_cast<Affinity>(networkAffinity),
                                 static_cast<Affinity>(rackAffinity));
                break;
            }
            case 2: {
                int noVMs, typeIndex, pgIndex, partition;
                std::cin >> noVMs >> typeIndex >> pgIndex >> partition;

                log << "Create VM(s)\n"
                    << "noVMs = " << noVMs
                    << ", typeIndex = " << typeIndex
                    << ", pgIndex = " << pgIndex
                    << ", partition = " << partition
                    << "\n----------"
                    << std::endl;

                std::vector<int> indices(noVMs);
                for (int j = 0; j < noVMs; j++) {
                    std::cin >> indices[j];
                }

                terminate = !manager.createVMs(indices, typeIndex, pgIndex, partition);
                break;
            }
            case 3: {
                int noVMs;
                std::cin >> noVMs;

                log << "Delete VM(s)\n"
                    << "noVMs = " << noVMs
                    << "\n----------"
                    << std::endl;

                std::vector<int> indices(noVMs);
                for (int j = 0; j < noVMs; j++) {
                    std::cin >> indices[j];
                }

                manager.deleteVMs(indices);
                break;
            }
            case 4: {
                log << "Terminate\n\"----------" << std::endl;

                terminate = true;
                break;
            }
            default: {
                log << "Unknown request type: " << requestType << std::endl;
            }
        }

        if (terminate) {
            log << "Terminating..." << std::endl;
            break;
        }
    }

    return 0;
}
