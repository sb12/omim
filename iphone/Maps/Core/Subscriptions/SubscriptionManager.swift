@objc protocol SubscriptionManagerListener: AnyObject {
  func didFailToSubscribe(_ subscription: ISubscription, error: Error?)
  func didSubsribe(_ subscription: ISubscription)
  func didDefer(_ subscription: ISubscription)
  func didFailToValidate()
  func didValidate(_ isValid: Bool)
}

class SubscriptionManager: NSObject {
  typealias SuscriptionsCompletion = ([ISubscription]?, Error?) -> Void
  typealias ValidationCompletion = (MWMValidationResult) -> Void

  private let paymentQueue = SKPaymentQueue.default()
  private var productsRequest: SKProductsRequest?
  private var subscriptionsComplection: SuscriptionsCompletion?
  private var products: [String: SKProduct]?
  private var pendingSubscription: ISubscription?
  private var listeners = NSHashTable<SubscriptionManagerListener>.weakObjects()
  private var restorationCallback: ValidationCompletion?

  private var productIds: [String] = []
  private var serverId: String = ""
  private var vendorId: String = ""
  private var purchaseManager: MWMPurchaseManager?

  convenience init(productIds: [String], serverId: String, vendorId: String) {
    self.init()
    self.productIds = productIds
    self.serverId = serverId
    self.vendorId = vendorId
    self.purchaseManager = MWMPurchaseManager(vendorId: vendorId)
  }

  override private init() {
    super.init()
    paymentQueue.add(self)
  }

  deinit {
    paymentQueue.remove(self)
  }

  @objc static func canMakePayments() -> Bool {
    return SKPaymentQueue.canMakePayments()
  }

  @objc func getAvailableSubscriptions(_ completion: @escaping SuscriptionsCompletion){
    subscriptionsComplection = completion
    productsRequest = SKProductsRequest(productIdentifiers: Set(productIds))
    productsRequest!.delegate = self
    productsRequest!.start()
  }

  @objc func subscribe(to subscription: ISubscription) {
    pendingSubscription = subscription
    guard let product = products?[subscription.productId] else { return }
    paymentQueue.add(SKPayment(product: product))
  }

  @objc func addListener(_ listener: SubscriptionManagerListener) {
    listeners.add(listener)
  }

  @objc func removeListener(_ listener: SubscriptionManagerListener) {
    listeners.remove(listener)
  }

  @objc func validate(completion: ValidationCompletion? = nil) {
    validate(false, completion: completion)
  }

  @objc func restore(_ callback: @escaping ValidationCompletion) {
    validate(true) {
      callback($0)
    }
  }

  private func validate(_ refreshReceipt: Bool, completion: ValidationCompletion? = nil) {
    purchaseManager?.validateReceipt(serverId, refreshReceipt: refreshReceipt) { [weak self] (_, validationResult) in
      self?.logEvents(validationResult)
      if validationResult == .valid || validationResult == .notValid {
        self?.listeners.allObjects.forEach { $0.didValidate(validationResult == .valid) }
        self?.paymentQueue.transactions
          .filter { self?.productIds.contains($0.payment.productIdentifier) ?? false &&
            ($0.transactionState == .purchased || $0.transactionState == .restored) }
          .forEach { self?.paymentQueue.finishTransaction($0) }
      } else {
        self?.listeners.allObjects.forEach { $0.didFailToValidate() }
      }
      completion?(validationResult)
    }
  }

  private func logEvents(_ validationResult: MWMValidationResult) {
    switch validationResult {
    case .valid:
      Statistics.logEvent(kStatInappValidationSuccess)
      Statistics.logEvent(kStatInappProductDelivered,
                          withParameters: [kStatVendor : vendorId])
    case .notValid:
      Statistics.logEvent(kStatInappValidationError, withParameters: [kStatErrorCode : 0])
    case .serverError:
      Statistics.logEvent(kStatInappValidationError, withParameters: [kStatErrorCode : 2])
    case .authError:
      Statistics.logEvent(kStatInappValidationError, withParameters: [kStatErrorCode : 1])
    }
  }
}

extension SubscriptionManager: SKProductsRequestDelegate {
  func request(_ request: SKRequest, didFailWithError error: Error) {
    Statistics.logEvent(kStatInappPaymentError,
                        withParameters: [kStatError : error.localizedDescription])
    subscriptionsComplection?(nil, error)
    subscriptionsComplection = nil
    productsRequest = nil
  }

  func productsRequest(_ request: SKProductsRequest, didReceive response: SKProductsResponse) {
    guard response.products.count == productIds.count else {
      subscriptionsComplection?(nil, NSError(domain: "mapsme.subscriptions", code: -1, userInfo: nil))
      subscriptionsComplection = nil
      productsRequest = nil
      return
    }
    let subscriptions = response.products.map { Subscription($0) }
      .sorted { $0.period.rawValue < $1.period.rawValue }
    products = Dictionary(uniqueKeysWithValues: response.products.map { ($0.productIdentifier, $0) })
    subscriptionsComplection?(subscriptions, nil)
    subscriptionsComplection = nil
    productsRequest = nil
  }
}

extension SubscriptionManager: SKPaymentTransactionObserver {
  func paymentQueue(_ queue: SKPaymentQueue, updatedTransactions transactions: [SKPaymentTransaction]) {
    let subscriptionTransactions = transactions.filter {
      productIds.contains($0.payment.productIdentifier)
    }
    subscriptionTransactions
      .filter { $0.transactionState == .failed }
      .forEach { processFailed($0, error: $0.error) }

    if subscriptionTransactions.contains(where: {
      $0.transactionState == .purchased || $0.transactionState == .restored
    }) {
      subscriptionTransactions
        .filter { $0.transactionState == .purchased }
        .forEach { processPurchased($0) }
      subscriptionTransactions
        .filter { $0.transactionState == .restored }
        .forEach { processRestored($0) }
      validate(false)
    }

    subscriptionTransactions
      .filter { $0.transactionState == .deferred }
      .forEach { processDeferred($0) }
  }

  private func processDeferred(_ transaction: SKPaymentTransaction) {
    if let ps = pendingSubscription, transaction.payment.productIdentifier == ps.productId {
      listeners.allObjects.forEach { $0.didDefer(ps) }
    }
  }

  private func processPurchased(_ transaction: SKPaymentTransaction) {
    paymentQueue.finishTransaction(transaction)
    if let ps = pendingSubscription, transaction.payment.productIdentifier == ps.productId {
      Statistics.logEvent(kStatInappPaymentSuccess)
      listeners.allObjects.forEach { $0.didSubsribe(ps) }
    }
  }

  private func processRestored(_ transaction: SKPaymentTransaction) {
    paymentQueue.finishTransaction(transaction)
    if let ps = pendingSubscription, transaction.payment.productIdentifier == ps.productId {
      listeners.allObjects.forEach { $0.didSubsribe(ps) }
    }
  }

  private func processFailed(_ transaction: SKPaymentTransaction, error: Error?) {
    paymentQueue.finishTransaction(transaction)
    if let ps = pendingSubscription, transaction.payment.productIdentifier == ps.productId {
      Statistics.logEvent(kStatInappPaymentError,
                          withParameters: [kStatError : error?.localizedDescription ?? ""])
      listeners.allObjects.forEach { $0.didFailToSubscribe(ps, error: error) }
    }
  }
}
